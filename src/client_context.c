#include "client_context.h"

static void ctx_free(client_ctx_t *ctx);

client_ctx_t *ctx_table[CTX_HASH_SIZE] = {0};
pthread_mutex_t ctx_table_locks[CTX_HASH_SIZE] = {
    [0 ... CTX_HASH_SIZE-1] = PTHREAD_MUTEX_INITIALIZER
};

// ws_cli_conn_t — это указатель, хэшируем его адрес
static inline int ctx_hash(ws_cli_conn_t client) {
    return ((uintptr_t)client >> 4) & (CTX_HASH_SIZE - 1);
}

void ctx_unref(client_ctx_t *ctx)
{
    if (!ctx) return;
    // fetch_sub возвращает значение ДО вычитания
    if (atomic_fetch_sub(&ctx->ref_count, 1) == 1) {
        ctx_free(ctx);
    }
}

client_ctx_t *get_ctx_by_client(ws_cli_conn_t client)
{
    int slot = ctx_hash(client);
    pthread_mutex_lock(&ctx_table_locks[slot]);

    client_ctx_t *curr = ctx_table[slot];
    while (curr) {
        if (curr->ws_conn == client) {
            atomic_fetch_add(&curr->ref_count, 1);
            pthread_mutex_unlock(&ctx_table_locks[slot]);
            return curr;
        }
        curr = curr->next;
    }

    pthread_mutex_unlock(&ctx_table_locks[slot]);
    return NULL;
}

void create_ctx(ws_cli_conn_t client, int target_fd)
{
    int slot = ctx_hash(client);

    client_ctx_t *new_node = malloc(sizeof(client_ctx_t));
    if (!new_node) return;

    new_node->ws_conn    = client;
    new_node->target_fd  = target_fd;
    new_node->is_handshaked = true;
    atomic_init(&new_node->ref_count, 1);
    atomic_init(&new_node->is_closing, false);
    atomic_init(&new_node->state, CTX_STATE_CONNECTING);
    pthread_mutex_init(&new_node->ws_send_lock, NULL);
    pthread_mutex_init(&new_node->pending_lock, NULL);
    new_node->pending_msgs  = NULL;
    new_node->pending_sizes = NULL;
    new_node->pending_count = 0;

    pthread_mutex_lock(&ctx_table_locks[slot]);
    new_node->next    = ctx_table[slot];
    ctx_table[slot]   = new_node;
    pthread_mutex_unlock(&ctx_table_locks[slot]);
}

// Буферизуем пакет пришедший пока соединение устанавливается
void ctx_enqueue_pending(client_ctx_t *ctx, const unsigned char *msg, size_t size)
{
    pthread_mutex_lock(&ctx->pending_lock);

    ctx->pending_msgs  = realloc(ctx->pending_msgs,
                                 (ctx->pending_count + 1) * sizeof(unsigned char *));
    ctx->pending_sizes = realloc(ctx->pending_sizes,
                                 (ctx->pending_count + 1) * sizeof(size_t));

    unsigned char *copy = malloc(size);
    memcpy(copy, msg, size);
    ctx->pending_msgs[ctx->pending_count]  = copy;
    ctx->pending_sizes[ctx->pending_count] = size;
    ctx->pending_count++;

    pthread_mutex_unlock(&ctx->pending_lock);
}

// Сбрасываем накопленные пакеты в сокет после успешного connect
void ctx_flush_pending(client_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->pending_lock);

    for (int i = 0; i < ctx->pending_count; i++) {
        send(ctx->target_fd, ctx->pending_msgs[i], ctx->pending_sizes[i], 0);
        free(ctx->pending_msgs[i]);
    }
    free(ctx->pending_msgs);
    free(ctx->pending_sizes);
    ctx->pending_msgs  = NULL;
    ctx->pending_sizes = NULL;
    ctx->pending_count = 0;

    pthread_mutex_unlock(&ctx->pending_lock);
}

void remove_ctx(ws_cli_conn_t client)
{
    int slot = ctx_hash(client);

    pthread_mutex_lock(&ctx_table_locks[slot]);

    client_ctx_t **curr = &ctx_table[slot];
    while (*curr) {
        client_ctx_t *entry = *curr;
        if (entry->ws_conn == client) {
            *curr = entry->next;
            pthread_mutex_unlock(&ctx_table_locks[slot]);
            printf("Context removed from list\n");
            ctx_unref(entry);
            return;
        }
        curr = &entry->next;
    }

    pthread_mutex_unlock(&ctx_table_locks[slot]);
}

bool ctx_send_bin(client_ctx_t *ctx, const unsigned char *data, size_t len)
{
    pthread_mutex_lock(&ctx->ws_send_lock);

    if (atomic_load(&ctx->is_closing)) {
        pthread_mutex_unlock(&ctx->ws_send_lock);
        return false;
    }

    ws_sendframe_bin(ctx->ws_conn, (char *)data, len);

    pthread_mutex_unlock(&ctx->ws_send_lock);
    return true;
}

static void ctx_free(client_ctx_t *ctx)
{
    pthread_mutex_destroy(&ctx->ws_send_lock);
    free(ctx);
    printf("Context freed\n");
}