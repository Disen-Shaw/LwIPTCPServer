
#pragma once

#include "lwip/err.h"
#include "lwip/api.h"
#include "lwip/mem.h"
#include "cmsis_os2.h"
#include <stdbool.h>

#define TCP_SERVER_ACTIVE_TIMEOUT_S     (30)

enum tcp_conn_state {
        TCP_CONN_STATE_INIT = 0,
        TCP_CONN_STATE_ACTIVE,
        TCP_CONN_STATE_CLOSING,
        TCP_CONN_STATE_CLOSED
};

struct tcp_server;

struct tcp_conn {
        struct netconn *conn;
        struct tcp_server *server;
        
        osThreadId_t thread_id;
        void *user_data;
        
        uint32_t last_active_tick;
        bool is_closing;
        enum tcp_conn_state state;
};

struct tcp_server_cbs {
        void (*on_connect)(struct tcp_conn *conn);
        void (*on_data)(struct tcp_conn *conn, struct netbuf *buf);
        void (*on_close)(struct tcp_conn *conn);
};

struct tcp_server {
        struct netconn *listen_conn;
        const struct tcp_server_cbs *cbs;
        uint16_t port;
        osThreadId_t server_monitor;
        
        osMutexId_t list_mutex;
        uint16_t active_conns;
        uint16_t max_conns;
};

err_t tcp_server_init(struct tcp_server *server,  
                      uint16_t port, uint16_t max_conns, 
                      const struct tcp_server_cbs *cbs);
err_t tcp_server_start(struct tcp_server *server);
err_t tcp_conn_send(struct tcp_conn *conn, const void *data, size_t len);

