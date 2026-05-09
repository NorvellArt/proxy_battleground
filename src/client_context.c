#include "client_context.h"

static client_ctx_t *get_ctx_inner(ws_cli_conn_t client);

client_ctx_t *ctx_list = NULL;
pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;

client_ctx_t *get_ctx_by_client(ws_cli_conn_t client)
{
    pthread_mutex_lock(&list_lock);
    client_ctx_t *res = get_ctx_inner(client); // Используем внутреннюю логику
    pthread_mutex_unlock(&list_lock);
    return res;
}

void create_ctx(ws_cli_conn_t client, int target_fd)
{
    pthread_mutex_lock(&list_lock);

    client_ctx_t *new_node = malloc(sizeof(client_ctx_t));
    if (!new_node) return;

    new_node->ws_conn = client;
    new_node->target_fd = target_fd;
    new_node->is_handshaked = true;
    
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
            if (entry->target_fd != -1) {
                close(entry->target_fd);
            }
            free(entry);
            printf("Context removed successfully\n");
            pthread_mutex_unlock(&list_lock);
            return;
        }
        curr = &entry->next;
    }

    pthread_mutex_unlock(&list_lock);
}

static client_ctx_t *get_ctx_inner(ws_cli_conn_t client) {
    client_ctx_t *curr = ctx_list;
    while (curr) {
        if (curr->ws_conn == client) return curr;
        curr = curr->next;
    }
    return NULL;
}
