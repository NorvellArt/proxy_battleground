#include "client_context.h"

static client_ctx_t *get_ctx_inner(ws_cli_conn_t client);
static void ctx_free(client_ctx_t *ctx);

client_ctx_t *ctx_list = NULL;
pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_lock(&list_lock);
    client_ctx_t *res = get_ctx_inner(client);
    if (res) {
        atomic_fetch_add(&res->ref_count, 1); // берём владение
    }
    pthread_mutex_unlock(&list_lock);
    return res; // вызывающий должен сделать ctx_unref() после использования
}

void create_ctx(ws_cli_conn_t client, int target_fd)
{
    pthread_mutex_lock(&list_lock);

    client_ctx_t *new_node = malloc(sizeof(client_ctx_t));
    if (!new_node) {
        pthread_mutex_unlock(&list_lock);
        return;
    }

    new_node->ws_conn = client;
    new_node->target_fd = target_fd;
    new_node->is_handshaked = true;
    atomic_init(&new_node->ref_count, 1);
    atomic_init(&new_node->is_closing, false);
    atomic_init(&new_node->state, CTX_STATE_CONNECTING);
    pthread_mutex_init(&new_node->ws_send_lock, NULL);
    pthread_mutex_init(&new_node->pending_lock, NULL);
    new_node->pending_msgs   = NULL;
    new_node->pending_sizes  = NULL;
    new_node->pending_count  = 0;
    
    // Вставляем в начало списка
    new_node->next = ctx_list;
    ctx_list = new_node;

    pthread_mutex_unlock(&list_lock);
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
    pthread_mutex_lock(&list_lock);

    client_ctx_t **curr = &ctx_list;
    while (*curr) {
        client_ctx_t *entry = *curr;
        if (entry->ws_conn == client) {
            *curr = entry->next;  
            pthread_mutex_unlock(&list_lock);

            printf("Context removed from list\n");
            ctx_unref(entry);             // снимаем ссылку списка
            return;
        }
        curr = &entry->next;
    }

    pthread_mutex_unlock(&list_lock);
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

static client_ctx_t *get_ctx_inner(ws_cli_conn_t client) {
    client_ctx_t *curr = ctx_list;
    while (curr) {
        if (curr->ws_conn == client) return curr;
        curr = curr->next;
    }
    return NULL;
}

static void ctx_free(client_ctx_t *ctx)
{
    pthread_mutex_destroy(&ctx->ws_send_lock);
    free(ctx);
    printf("Context freed\n");
}