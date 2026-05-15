#ifndef CLIENT_CONTEXT_H
#define CLIENT_CONTEXT_H

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>

#include <ws.h>

typedef enum {
    CTX_STATE_CONNECTING, // воркер ещё не завершил connect
    CTX_STATE_CONNECTED,  // соединение установлено, можно форвардить
} ctx_state_t;

typedef struct client_ctx{
    int target_fd;
    bool is_handshaked;
    ws_cli_conn_t ws_conn;

    atomic_int ref_count;
    atomic_bool is_closing;
    pthread_mutex_t ws_send_lock;

    atomic_int    state;        // ctx_state_t — CTX_STATE_CONNECTING или CONNECTED

    // Буфер пакетов пришедших пока state == CONNECTING
    unsigned char **pending_msgs;
    size_t         *pending_sizes;
    int             pending_count;
    pthread_mutex_t pending_lock;

    struct client_ctx *next;
} client_ctx_t;

#define CTX_HASH_SIZE 256

extern client_ctx_t *ctx_table[CTX_HASH_SIZE];
extern pthread_mutex_t ctx_table_locks[CTX_HASH_SIZE];

client_ctx_t *get_ctx_by_client(ws_cli_conn_t client);

void ctx_enqueue_pending(client_ctx_t *ctx, const unsigned char *msg, size_t size);
void ctx_flush_pending(client_ctx_t *ctx);

void ctx_unref(client_ctx_t *ctx);
void create_ctx(ws_cli_conn_t client, int target_fd);
void remove_ctx(ws_cli_conn_t client);

bool ctx_send_bin(client_ctx_t *ctx, const unsigned char *data, size_t len);

#endif