#include <stdio.h>
#include <unistd.h>
#include <ws.h>
#include <arpa/inet.h>
#include <string.h>

void onopen(ws_cli_conn_t client)
{
    char *cli;
    cli = ws_getaddress(client);
    printf("Connection opened, addr: %s\n", cli);
}

void onclose(ws_cli_conn_t client)
{
    char *cli;
    cli = ws_getaddress(client);
    printf("Connection closed, addr: %s\n", cli);
}

void onmessage(ws_cli_conn_t client, const unsigned char *msg, uint64_t size, int type)
{
    if (type != WS_FR_OP_BIN) return;

    if (size < 20) return;

    uint8_t version = msg[0];

    uint8_t addons_len = msg[17];
    int cursor = 18 + addons_len;

    uint8_t cmd = msg[cursor];
    cursor++;

    uint16_t port = (msg[cursor] << 8) | msg[cursor + 1];
    cursor += 2;

    uint8_t addr_type = msg[cursor];
    cursor++;

    char target_addr[256];

    if (addr_type == 0x01) {
        struct in_addr addr;
        memcpy(&addr, &msg[cursor], 4);
        inet_ntop(AF_INET, &addr, target_addr, sizeof(target_addr));
        cursor += 4;
    } 
    else if (addr_type == 0x02) {
        uint8_t host_len = msg[cursor];
        cursor++;
        memcpy(target_addr, &msg[cursor], host_len);
        target_addr[host_len] = '\0';
        cursor += host_len;
    }

    printf("Target: %s:%d (Command: %d)\n", target_addr, port, cmd);
}

int main()
{
    ws_socket(&(struct ws_server){
        .host = "0.0.0.0",
        .port = 7777,
        .thread_loop   = 0,
        .timeout_ms    = 1000,
        .evs.onopen    = &onopen,
        .evs.onclose   = &onclose,
        .evs.onmessage = &onmessage
    });

    return (0);
}