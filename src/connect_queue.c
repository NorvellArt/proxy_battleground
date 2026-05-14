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

    pthread_cond_signal(&q->cond);    // будим одного воркера
    pthread_mutex_unlock(&q->lock);
}

// Блокируется пока нет задач или не пришёл shutdown
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

// Воркер: берёт задачу, делает getaddrinfo + connect в фоне
static void *connect_worker(void *arg)
{
    worker_arg_t *warg = (worker_arg_t *)arg;
    int epoll_fd = warg->epoll_fd;
    free(warg);

    while (1) {
        connect_task_t *task = connect_queue_pop(&g_connect_queue);
        if (!task) break;   // shutdown

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

// Отдельная функция — нет goto через объявления, нет утечки
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

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        printf("[worker] connect() failed: %s\n", strerror(errno));
        perror("Connect failed");
        freeaddrinfo(res);
        close(sock);
        goto done;
    }
    printf("[worker] connected fd=%d\n", sock);

    freeaddrinfo(res);
    fcntl(sock, F_SETFL, O_NONBLOCK);


    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    client_ctx_t *ctx = get_ctx_by_client(task->client);
    if (!ctx) {
        // WS-клиент уже отключился пока мы делали DNS+connect
        printf("[worker] client gone after connect, closing fd=%d\n", sock);
        close(sock);
        goto done;
    }

    ctx->target_fd = sock;
    
    printf("[worker] ctx ok, state -> CONNECTED\n");

    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = ctx};
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);

    unsigned char vless_resp[] = {0x00, 0x00};
    ws_sendframe_bin(task->client, (char *)vless_resp, 2);
    printf("[worker] vless_resp sent\n");

    if (task->payload_len > 0) {
        ssize_t s = send(sock, task->payload, task->payload_len, 0);
        printf("[worker] payload sent: %zd bytes\n", s);
    }

    atomic_store(&ctx->state, CTX_STATE_CONNECTED);
    ctx_flush_pending(ctx);
    printf("[worker] pending flushed\n");

    ctx_unref(ctx);

done:
    free((void *)task->payload);
    free(task);
}