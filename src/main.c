#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <netdb.h>

#include "main.h"

int epoll_fd;

// Поток для чтения ответов от сайтов (например, от Google) и отправки их в телефон
void *epoll_loop_thread(void *arg) {
    struct epoll_event events[MAX_EVENTS];
    unsigned char *buffer = malloc(BUF_SIZE);

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < nfds; i++) {
            client_ctx_t *ctx = (client_ctx_t *)events[i].data.ptr;

            ssize_t n = recv(ctx->target_fd, buffer, BUF_SIZE, 0);
            if (n > 0) {
                // Данные от сайта -> в WebSocket (телефон)
                ws_sendframe_bin(ctx->ws_conn, buffer, n);
            } else {
                // Ошибка или закрытие соединения со стороны сайта
                printf("Target closed connection\n");
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->target_fd, NULL);
                close(ctx->target_fd);
                // Тут стоило бы закрыть и ws-соединение, но для примера оставим так
                free(ctx);
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
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->target_fd, NULL);
        remove_ctx(client);
    }
}

void onmessage(ws_cli_conn_t client, const unsigned char *msg, uint64_t size, int type)
{
    if (size == 0) return;

    client_ctx_t *ctx = get_ctx_by_client(client);

    if (ctx && ctx->is_handshaked) {
        send(ctx->target_fd, msg, size, 0);
        return;
    }

    if (type != WS_FR_OP_BIN) return;

    // В учебном примере считаем, что первый пакет всегда содержит заголовок VLESS.
    // В реальном прокси нужно хранить состояние 'handshaked' в контексте клиента.
    
    if (size < 20) return; // Минимум для VLESS

    int cursor = 0;
    uint8_t ver = msg[cursor++];     // 0
    cursor += 16;                    // Пропускаем UUID
    uint8_t a_len = msg[cursor++];   // Читаем длину дополнений
    cursor += a_len;                 // Прыгаем через дополнения
    
    uint8_t cmd = msg[cursor++];     // Команда
    uint16_t port = (msg[cursor] << 8) | msg[cursor + 1];
    cursor += 2;
    
    uint8_t addr_type = msg[cursor++];
    char target_addr[256];

    if (addr_type == 0x01) { // IPv4
        inet_ntop(AF_INET, &msg[cursor], target_addr, sizeof(target_addr));
        cursor += 4;
    } else if (addr_type == 0x02) { // Domain
        uint8_t len = msg[cursor++];
            if (len > 0 && len < 255) {
            memcpy(target_addr, &msg[cursor], len);
            target_addr[len] = '\0';
            cursor += len;
        } else {
            strcpy(target_addr, "invalid-domain");
        }
    }

    printf("Connecting to %s:%d\n", target_addr, port);

    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);

    // DNS-резолвинг (блокирующий, но решает проблему доменов)
    if (getaddrinfo(target_addr, port_str, &hints, &res) != 0) {
        return; 
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

   if (connect(sock, res->ai_addr, res->ai_addrlen) == 0) {
        freeaddrinfo(res); // Освобождаем память DNS
        fcntl(sock, F_SETFL, O_NONBLOCK);

        // Сначала создаем контекст, чтобы на следующие пакеты ctx уже был готов
        create_ctx(client, sock); 
        client_ctx_t *new_ctx = get_ctx_by_client(client);
        if (new_ctx) {
            new_ctx->is_handshaked = true;

            // Регистрируем в epoll, чтобы начать слушать ответы от сайта
            struct epoll_event ev = {.events = EPOLLIN, .data.ptr = new_ctx};
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);
        }

        // Теперь подтверждаем клиенту, что соединение готово
        unsigned char vless_resp[] = {0x00, 0x00};
        ws_sendframe_bin(client, vless_resp, 2);

        // 4. Пробрасываем полезную нагрузку (payload), если она была в первом пакете
        if (size > cursor) {
            send(sock, &msg[cursor], size - cursor, 0);
        }
    } else {
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