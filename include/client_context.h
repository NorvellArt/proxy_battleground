#ifndef CLIENT_CONTEXT_H
#define CLIENT_CONTEXT_H

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>

#include <ws.h>

typedef struct client_ctx{
    int target_fd;
    bool is_handshaked;
    ws_cli_conn_t ws_conn;

    atomic_int ref_count;
    atomic_bool is_closing;

    struct client_ctx *next;
} client_ctx_t;

extern client_ctx_t *ctx_list;
extern pthread_mutex_t list_lock;

client_ctx_t *get_ctx_by_client(ws_cli_conn_t client);

void ctx_unref(client_ctx_t *ctx);
void create_ctx(ws_cli_conn_t client, int target_fd);
void remove_ctx(ws_cli_conn_t client);

#endif