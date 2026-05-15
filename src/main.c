#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>

#include "main.h"
#include "connect_queue.h"

#define CONNECT_WORKERS 4

int epoll_fd;

void *epoll_loop_thread(void *arg) {
    int epoll_fd = *(int *)arg;
    struct epoll_event events[MAX_EVENTS];
    unsigned char *buffer = malloc(BUF_SIZE);

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < nfds; i++) {
            client_ctx_t *ctx = (client_ctx_t *)events[i].data.ptr;
            if (!ctx) continue;
            if (atomic_load(&ctx->is_closing)) continue;

            bool closed = false;

            /* drain-loop: с EPOLLET вычитываем всё до EAGAIN */
            while (1) {
                ssize_t n = recv(ctx->target_fd, buffer, BUF_SIZE, 0);
                if (n > 0) {
                    ctx_send_bin(ctx, buffer, n);
                    continue;
                }
                if (n == 0) {
                    bool expected = false;
                    if (atomic_compare_exchange_strong(&ctx->is_closing, &expected, true)) {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->target_fd, NULL);
                        ws_close_client(ctx->ws_conn);
                    }
                    closed = true;
                    break;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                bool expected = false;
                if (atomic_compare_exchange_strong(&ctx->is_closing, &expected, true)) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->target_fd, NULL);
                    ws_close_client(ctx->ws_conn);
                }
                closed = true;
                break;
            }

            /* Переармируем EPOLLONESHOT — иначе fd больше не будет в epoll */
            if (!closed && !atomic_load(&ctx->is_closing)) {
                struct epoll_event ev = {
                    .events = EPOLLIN | EPOLLET | EPOLLONESHOT,
                    .data.ptr = ctx
                };
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ctx->target_fd, &ev);
            }
        }
    }

    free(buffer);
    return NULL;
}

void onopen(ws_cli_conn_t client)
{
    char *cli = ws_getaddress(client);
    printf("Connection opened, addr: %s\n", cli);
}

void onclose(ws_cli_conn_t client)
{
    printf("Connection closed\n");
    client_ctx_t *ctx = get_ctx_by_client(client);

    if (!ctx) {
        printf("onclose: context already removed\n");
        return;
    }

    pthread_mutex_lock(&ctx->ws_send_lock);
    atomic_store(&ctx->is_closing, true);
    pthread_mutex_unlock(&ctx->ws_send_lock);

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->target_fd, NULL);
    close(ctx->target_fd);

    ctx_unref(ctx);
    remove_ctx(client);
}

void onmessage(ws_cli_conn_t client, const unsigned char *msg, uint64_t size, int type)
{
    if (size == 0) return;

    client_ctx_t *ctx = get_ctx_by_client(client);

    if (ctx) {
        if (!atomic_load(&ctx->is_closing)) {
            pthread_mutex_lock(&ctx->pending_lock);
            if (atomic_load(&ctx->state) == CTX_STATE_CONNECTING) {
                unsigned char *copy = malloc(size);
                memcpy(copy, msg, size);
                ctx->pending_msgs  = realloc(ctx->pending_msgs,
                                             (ctx->pending_count + 1) * sizeof(unsigned char *));
                ctx->pending_sizes = realloc(ctx->pending_sizes,
                                             (ctx->pending_count + 1) * sizeof(size_t));
                ctx->pending_msgs[ctx->pending_count]  = copy;
                ctx->pending_sizes[ctx->pending_count] = size;
                ctx->pending_count++;
            } else {
                /* safe_send: не теряем байты при заполненном буфере ядра */
                safe_send(ctx->target_fd, msg, size);
            }
            pthread_mutex_unlock(&ctx->pending_lock);
        }
        ctx_unref(ctx);
        return;
    }

    /* --- Парсинг нового VLESS-соединения --- */
    if (type != WS_FR_OP_BIN) return;
    if (size < 20) {
        printf("DEBUG: Packet too small for VLESS: %lu\n", size);
        return;
    }

    int cursor = 0;
    uint8_t ver = msg[cursor++];
    cursor += 16;                  /* UUID */
    uint8_t a_len = msg[cursor++];

    if (cursor + a_len >= size) return;
    cursor += a_len;

    uint8_t cmd = msg[cursor++];
    uint16_t port = (msg[cursor] << 8) | msg[cursor + 1];
    cursor += 2;

    uint8_t addr_type = msg[cursor++];
    char target_addr[256] = {0};

    if (addr_type == 0x01) { /* IPv4 */
        inet_ntop(AF_INET, &msg[cursor], target_addr, sizeof(target_addr));
        cursor += 4;
    } else if (addr_type == 0x02) { /* Domain */
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

    size_t payload_len = (size > (size_t)cursor) ? size - cursor : 0;
    uint8_t *payload_copy = NULL;

    if (payload_len > 0) {
        payload_copy = malloc(payload_len);
        if (!payload_copy) return;
        memcpy(payload_copy, &msg[cursor], payload_len);
    }

    connect_task_t *task = malloc(sizeof(connect_task_t));
    if (!task) { free(payload_copy); return; }

    task->client      = client;
    task->port        = port;
    task->payload     = payload_copy;
    task->payload_len = payload_len;
    memcpy(task->target_addr, target_addr, sizeof(target_addr));

    create_ctx(client, -1);
    connect_queue_push(&g_connect_queue, task);
}

int main()
{
    epoll_fd = epoll_create1(0);
    printf("[main] epoll_fd=%d\n", epoll_fd);

    connect_workers_start(CONNECT_WORKERS, epoll_fd);

    for (int i = 0; i < EPOLL_WORKERS; i++) {
        pthread_t t;
        pthread_create(&t, NULL, epoll_loop_thread, &epoll_fd);
        pthread_detach(t);
    }

    ws_socket(&(struct ws_server){
        .host = "0.0.0.0",
        .port = 7777,
        .thread_loop   = 0,
        .timeout_ms    = 1000,
        .evs.onopen    = &onopen,
        .evs.onclose   = &onclose,
        .evs.onmessage = &onmessage
    });

    return 0;
}