
#pragma once

#include "lwip/tcpip.h"
#include "lwip/tcp.h"
#include "cmsis_os2.h"
#include <stdbool.h>

struct tcp_server;

struct tcp_connection {
        struct tcp_pcb *pcb;
        struct tcp_server *server;
        struct tcp_connection *next;
        
        uint32_t connection_id;
        void *user_data;
        uint32_t last_active_tick;
        
        volatile uint8_t state;
        uint16_t unacked_len;
};

struct tcp_server_cbs {
        void (*on_connect)(struct tcp_connection *conn);
        void (*on_data)(struct tcp_connection *conn, struct pbuf *p);
        void (*on_sent)(struct tcp_connection *conn, uint16_t len);
        void (*on_error)(struct tcp_connection *conn, err_t err);
        void (*on_disconnect)(struct tcp_connection *conn);
};

struct tcp_server {
        struct tcp_pcb *listen_pcb;
        struct tcp_connection *conn_list;
        uint16_t port;
        
        uint16_t max_connection;
        uint16_t active_conection;
        
        osMemoryPoolId_t conn_pool_id;
        osMemoryPoolId_t req_message_id;
        osMutexId_t server_lock;
        
        const struct tcp_server_cbs *cbs;
};

int tcp_server_init(struct tcp_server *server, uint16_t port, uint16_t max_conn, const struct tcp_server_cbs *cbs);
void tcp_server_close(struct tcp_server *server);
void tcp_server_send(struct tcp_connection *conn, const void *data, uint16_t len);
bool tcp_server_conn_is_alive(const struct tcp_connection *conn);
void tcp_server_conn_close(struct tcp_connection *conn);
