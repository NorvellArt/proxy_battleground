#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

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
        printf("DNS resolution failed for %s\n", task->target_addr);
        goto done;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        goto done;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        perror("Connect failed");
        freeaddrinfo(res);
        close(sock);
        goto done;
    }

    freeaddrinfo(res);
    fcntl(sock, F_SETFL, O_NONBLOCK);

    create_ctx(task->client, sock);

    client_ctx_t *ctx = get_ctx_by_client(task->client);
    if (ctx) {
        struct epoll_event ev = {.events = EPOLLIN, .data.ptr = ctx};
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);
        ctx_unref(ctx);
    }

    unsigned char vless_resp[] = {0x00, 0x00};
    ws_sendframe_bin(task->client, (char *)vless_resp, 2);

    if (task->payload_len > 0) {
        send(sock, task->payload, task->payload_len, 0);
    }

done:
    free((void *)task->payload);
    free(task);
}