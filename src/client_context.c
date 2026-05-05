#include "client_context.h"

static client_ctx_t *ctx_list = NULL;

client_ctx_t *get_ctx_by_client(ws_cli_conn_t client)
{
    printf("Searching for ctx. Head is %p\n", (void*)ctx_list);
    client_ctx_t *curr = ctx_list;
    int i = 0;
    while (curr) {
        printf("Iteration %d: curr=%p, next=%p\n", i++, (void*)curr, (void*)curr->next);
        
        // Если упадет здесь, значит curr — битый указатель
        if (curr->ws_conn == client) return curr; 
        
        curr = curr->next;
    }
    return NULL;
}

void create_ctx(ws_cli_conn_t client, int target_fd)
{
    client_ctx_t *new_node = malloc(sizeof(client_ctx_t));
    if (!new_node) return;

    new_node->ws_conn = client;
    new_node->target_fd = target_fd;
    new_node->is_handshaked = false;
    
    // Вставляем в начало списка
    new_node->next = ctx_list;
    ctx_list = new_node;
}

void remove_ctx(ws_cli_conn_t client)
{
    client_ctx_t **curr = &ctx_list;
    while (*curr) {
        client_ctx_t *entry = *curr;
        if (entry->ws_conn == client) {
            *curr = entry->next;
            if (entry->target_fd != -1) close(entry->target_fd);
            free(entry);
            return;
        }
        curr = &entry->next;
    }
}
