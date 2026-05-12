#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <netdb.h>

#include "main.h"

int epoll_fd;

void *epoll_loop_thread(void *arg) {
    struct epoll_event events[MAX_EVENTS];
    unsigned char *buffer = malloc(BUF_SIZE);

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < nfds; i++) {
            client_ctx_t *ctx = (client_ctx_t *)events[i].data.ptr;
            
            if (!ctx) continue;
            
            // Проверяем флаг ДО любой работы с ctx
            if (atomic_load(&ctx->is_closing)) continue;

            ssize_t n = recv(ctx->target_fd, buffer, BUF_SIZE, 0);
            if (n > 0) {
                ctx_send_bin(ctx, buffer, n);
            } else {
                printf("Target closed connection or error\n");

                if (!atomic_load(&ctx->is_closing)) {
                    ws_close_client(ctx->ws_conn);
                }
            }
        }
    }
    free(buffer);
    return NULL;
}

void onopen(ws_cli_conn_t client)
{
    char *cli;
    cli = ws_getaddress(client);
    printf("Connection opened, addr: %s\n", cli);
}

void onclose(ws_cli_conn_t client)
{
    printf("Connection closed\n");
    client_ctx_t *ctx = get_ctx_by_client(client);
    
    if (ctx) {
        // Захватываем lock: ждём завершения текущего ws_sendframe_bin если он идёт
        pthread_mutex_lock(&ctx->ws_send_lock);
        atomic_store(&ctx->is_closing, true);  // теперь ctx_send_bin увидит флаг
        pthread_mutex_unlock(&ctx->ws_send_lock);

        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->target_fd, NULL);
        close(ctx->target_fd);

        ctx_unref(ctx);
        remove_ctx(client);
    }
}

void onmessage(ws_cli_conn_t client, const unsigned char *msg, uint64_t size, int type)
{
    if (size == 0) return;

    // --- 1. ПРОВЕРКА СУЩЕСТВУЮЩЕГО КОНТЕКСТА ---
    // Используем безопасную функцию с lock внутри
    client_ctx_t *ctx = get_ctx_by_client(client); 
    
    if (ctx) {
        if (ctx->is_handshaked) {
            ssize_t sent = send(ctx->target_fd, msg, size, 0);
            printf("DEBUG: Forwarded %zd bytes to target\n", sent);
        }
        ctx_unref(ctx);
        return; // Если контекст есть, дальше (на парсинг) не идем никогда
    }

    // --- 2. ПАРСИНГ НОВОГО СОЕДИНЕНИЯ (VLESS) ---
    if (type != WS_FR_OP_BIN) return;
    if (size < 20) {
        printf("DEBUG: Packet too small for VLESS: %lu\n", size);
        return;
    }

    int cursor = 0;
    uint8_t ver = msg[cursor++];     
    cursor += 16;                    // UUID
    uint8_t a_len = msg[cursor++];   
    
    // Проверка, чтобы cursor не вышел за пределы пакета
    if (cursor + a_len >= size) return;
    cursor += a_len;                 
    
    uint8_t cmd = msg[cursor++];     
    uint16_t port = (msg[cursor] << 8) | msg[cursor + 1];
    cursor += 2;
    
    uint8_t addr_type = msg[cursor++];
    char target_addr[256] = {0};

    if (addr_type == 0x01) { // IPv4
        inet_ntop(AF_INET, &msg[cursor], target_addr, sizeof(target_addr));
        cursor += 4;
    } else if (addr_type == 0x02) { // Domain
        uint8_t len = msg[cursor++];
        if (len > 0 && cursor + len <= size) {
            memcpy(target_addr, &msg[cursor], len);
            target_addr[len] = '\0';
            cursor += len;
        } else {
            return;
        }
    } else {
        printf("DEBUG: Unknown address type: %d\n", addr_type);
        return;
    }

    printf("Connecting to %s:%d\n", target_addr, port);

    // --- 3. УСТАНОВКА СОЕДИНЕНИЯ ---
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(target_addr, port_str, &hints, &res) != 0) {
        return; 
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) == 0) {
        freeaddrinfo(res);
        fcntl(sock, F_SETFL, O_NONBLOCK);

        // Создаем контекст (is_handshaked должен стать true внутри create_ctx)
        create_ctx(client, sock); 
        
        // Получаем созданный контекст для регистрации в epoll
        client_ctx_t *new_ctx = get_ctx_by_client(client);
        if (new_ctx) {
            struct epoll_event ev = {.events = EPOLLIN, .data.ptr = new_ctx};
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);
            ctx_unref(new_ctx); // отпускаем ссылку get_ctx_by_client
            // epoll держит указатель но НЕ ref — это нормально,
            // т.к. is_closing защищает от обращения после free
        }

        // Ответ клиенту (VLESS response)
        unsigned char vless_resp[] = {0x00, 0x00};
        ws_sendframe_bin(client, vless_resp, 2);

        // Пробрасываем остаток данных (payload)
        if (size > cursor) {
            send(sock, &msg[cursor], size - cursor, 0);
        }
    } else {
        perror("Connect failed");
        freeaddrinfo(res);
        close(sock);
    }
}

int main()
{
    // Создаем epoll
    epoll_fd = epoll_create1(0);

    // Запускаем поток для обработки ответов из интернета
    pthread_t thread;
    pthread_create(&thread, NULL, epoll_loop_thread, NULL);
    
    ws_socket(&(struct ws_server){
        .host = "0.0.0.0",
        .port = 7777,
        .thread_loop   = 0,
        .timeout_ms    = 1000,
        .evs.onopen    = &onopen,
        .evs.onclose   = &onclose,
        .evs.onmessage = &onmessage
    });

    return (0);
}