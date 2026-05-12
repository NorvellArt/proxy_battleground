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
    if (!new_node) return;

    new_node->ws_conn = client;
    new_node->target_fd = target_fd;
    new_node->is_handshaked = true;
    atomic_init(&new_node->ref_count, 1);    // список держит 1 ссылку
    atomic_init(&new_node->is_closing, false);
    pthread_mutex_init(&new_node->ws_send_lock, NULL);
    
    // Вставляем в начало списка
    new_node->next = ctx_list;
    ctx_list = new_node;

    pthread_mutex_unlock(&list_lock);
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