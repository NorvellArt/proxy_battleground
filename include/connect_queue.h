#ifndef CONNECT_QUEUE_H
#define CONNECT_QUEUE_H

#include <stdint.h>
#include <pthread.h>
#include <ws.h>

// Задача на установку соединения
typedef struct connect_task
{
    ws_cli_conn_t client;
    char target_addr[256];
    uint16_t port;
    const uint8_t *payload; // остаток VLESS-пакета после заголовка
    size_t payload_len;
    struct connect_task *next;
} connect_task_t;

// Очередь задач
typedef struct
{
    connect_task_t *head;
    connect_task_t *tail;
    pthread_mutex_t lock;
    pthread_cond_t cond; // сигнал воркерам о новой задаче
    int shutdown;        // флаг завершения
} connect_queue_t;

extern connect_queue_t g_connect_queue;

void connect_queue_init(connect_queue_t *q);
void connect_queue_push(connect_queue_t *q, connect_task_t *task);
connect_task_t *connect_queue_pop(connect_queue_t *q); // блокирует до задачи

// Запускает N воркер-потоков
void connect_workers_start(int n_workers, int epoll_fd);

#endif