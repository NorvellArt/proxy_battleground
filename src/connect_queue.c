#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>

#include "connect_queue.h"
#include "client_context.h"
#include "main.h"

connect_queue_t g_connect_queue;

typedef struct {
    connect_queue_t *queue;
    int epoll_fd;
} worker_arg_t;

static void connect_worker_handle(connect_task_t *task, int epoll_fd);

void connect_queue_init(connect_queue_t *q)
{
    q->head     = NULL;
    q->tail     = NULL;
    q->shutdown = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
}

void connect_queue_push(connect_queue_t *q, connect_task_t *task)
{
    task->next = NULL;
    pthread_mutex_lock(&q->lock);

    if (q->tail) {
        q->tail->next = task;
    } else {
        q->head = task;
    }
    q->tail = task;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

connect_task_t *connect_queue_pop(connect_queue_t *q)
{
    pthread_mutex_lock(&q->lock);

    while (!q->head && !q->shutdown) {
        pthread_cond_wait(&q->cond, &q->lock);
    }

    if (q->shutdown && !q->head) {
        pthread_mutex_unlock(&q->lock);
        return NULL;
    }

    connect_task_t *task = q->head;
    q->head = task->next;
    if (!q->head) q->tail = NULL;

    pthread_mutex_unlock(&q->lock);
    return task;
}

static void *connect_worker(void *arg)
{
    worker_arg_t *warg = (worker_arg_t *)arg;
    int epoll_fd = warg->epoll_fd;
    free(warg);

    while (1) {
        connect_task_t *task = connect_queue_pop(&g_connect_queue);
        if (!task) break;

        connect_worker_handle(task, epoll_fd);
    }

    return NULL;
}

void connect_workers_start(int n_workers, int epoll_fd)
{
    connect_queue_init(&g_connect_queue);
    for (int i = 0; i < n_workers; i++) {
        worker_arg_t *arg = malloc(sizeof(worker_arg_t));
        arg->queue    = &g_connect_queue;
        arg->epoll_fd = epoll_fd;
        pthread_t t;
        pthread_create(&t, NULL, connect_worker, arg);
        pthread_detach(t);
    }
}

static void connect_worker_handle(connect_task_t *task, int epoll_fd)
{
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", task->port);

    struct addrinfo hints = {0}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(task->target_addr, port_str, &hints, &res) != 0) {
        printf("[worker] DNS failed for %s\n", task->target_addr);
        goto done;
    }
    printf("[worker] DNS ok: %s:%u\n", task->target_addr, task->port);

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        printf("[worker] socket() failed\n");
        freeaddrinfo(res);
        goto done;
    }

    /* ---- Увеличиваем буферы ядра до подключения ---- */
    {
        int bufsize = 256 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        printf("[worker] connect() failed: %s\n", strerror(errno));
        freeaddrinfo(res);
        close(sock);
        goto done;
    }
    printf("[worker] connected fd=%d\n", sock);

    freeaddrinfo(res);

    /* Переводим в неблокирующий режим ПОСЛЕ connect */
    fcntl(sock, F_SETFL, O_NONBLOCK);

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    client_ctx_t *ctx = get_ctx_by_client(task->client);
    if (!ctx) {
        printf("[worker] client gone after connect, closing fd=%d\n", sock);
        close(sock);
        goto done;
    }

    ctx->target_fd = sock;
    printf("[worker] ctx ok, state -> CONNECTED\n");

    /* EPOLLONESHOT: после срабатывания fd автоматически отключается из epoll.
       Только один поток обработает событие, затем переармирует через MOD. */
    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLET | EPOLLONESHOT,
        .data.ptr = ctx
    };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);

    /* VLESS-ответ: используем safe_send (сокет ещё блокирующий не нужен,
       но safe_send корректно отработает и здесь) */
    unsigned char vless_resp[] = {0x00, 0x00};
    if (ctx_send_bin(ctx, vless_resp, 2) < 0) {
        printf("[worker] failed to send vless_resp\n");
        ctx_unref(ctx);
        goto done;
    }
    printf("[worker] vless_resp sent\n");

    if (task->payload_len > 0) {
        ssize_t s = safe_send(sock, task->payload, task->payload_len);
        printf("[worker] payload sent: %zd bytes\n", s);
    }

    /* Переход в CONNECTED и flush — под одним pending_lock */
    pthread_mutex_lock(&ctx->pending_lock);
    atomic_store(&ctx->state, CTX_STATE_CONNECTED);
    for (int i = 0; i < ctx->pending_count; i++) {
        safe_send(ctx->target_fd, ctx->pending_msgs[i], ctx->pending_sizes[i]);
        free(ctx->pending_msgs[i]);
    }
    free(ctx->pending_msgs);
    free(ctx->pending_sizes);
    ctx->pending_msgs  = NULL;
    ctx->pending_sizes = NULL;
    ctx->pending_count = 0;
    pthread_mutex_unlock(&ctx->pending_lock);

    printf("[worker] state -> CONNECTED, pending flushed\n");
    ctx_unref(ctx);

done:
    free((void *)task->payload);
    free(task);
}