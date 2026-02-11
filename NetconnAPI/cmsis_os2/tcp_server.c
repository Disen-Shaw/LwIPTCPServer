
#include "tcp_server.h"
#include <string.h>

#define TCP_SERVER_SEND_REQ_BUF_SIZE    (512)
#define TCP_SERVER_TIMEOUT_S            (20)

static void _internal_connection_free(struct tcp_connection *conn)
{
        if(!conn)
                return;
        
        struct tcp_server *server = conn->server;
        
        if(conn->pcb) {
                tcp_arg(conn->pcb, NULL);
                tcp_recv(conn->pcb, NULL);
                tcp_sent(conn->pcb, NULL);
                tcp_err(conn->pcb, NULL);
                tcp_poll(conn->pcb, NULL, 0);
        }
        
        conn->state = 0;
        
        osMutexAcquire(server->server_lock, osWaitForever);
        
        struct tcp_connection **pp = &server->conn_list;
        while(*pp) {
                if(*pp == conn) {
                        *pp = conn->next;
                        break;
                }
                pp = &(*pp)->next;
        }
        
        if(server->active_conection > 0) {
                server->active_conection--;
        }
        osMutexRelease(server->server_lock);
        osMemoryPoolFree(server->conn_pool_id, conn);
}

static void _lwip_error_cb(void *arg, err_t err)
{
        struct tcp_connection *conn = (struct tcp_connection *)arg;
        if(conn) {
                if(conn->server->cbs->on_error) {
                        conn->server->cbs->on_error(conn, err);
                }
                conn->pcb = NULL;
                _internal_connection_free(conn);
        }
}

static err_t _lwip_sent_cb(void *arg, struct tcp_pcb *tpcb, uint16_t len)
{
        struct tcp_connection *conn = (struct tcp_connection *)arg;
        if(conn && conn->state == 1) {
                if(conn->unacked_len >= len) {
                        conn->unacked_len -= len;
                }
                if(conn->server->cbs->on_sent) {
                        conn->server->cbs->on_sent(conn, len);
                }
        }
        
        return ERR_OK;
}

static err_t _lwip_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
        struct tcp_connection *conn = (struct tcp_connection *)arg;
        if(!conn)
                return ERR_VAL;
        
        if(p != NULL) {
                conn->last_active_tick = sys_now();
                tcp_recved(tpcb, p->tot_len);
                if(conn->server->cbs->on_data) {
                        conn->server->cbs->on_data(conn, p);
                }
                pbuf_free(p);
        } else if (err == ERR_OK) {
                if(conn->server->cbs->on_disconnect) {
                        conn->server->cbs->on_disconnect(conn);
                }
                tcp_arg(tpcb, NULL);
                tcp_close(tpcb);
                conn->pcb = NULL;
                _internal_connection_free(conn);
        } else {
                tcp_arg(tpcb, NULL);
                tcp_close(tpcb);
                conn->pcb = NULL;
                _internal_connection_free(conn);
        }
        return ERR_OK;
}

static err_t _lwip_poll_cb(void *arg, struct tcp_pcb *tpcb)
{
        struct tcp_connection *conn = (struct tcp_connection *)arg;
        if(!conn || !tpcb) {
                return ERR_OK;
        }
        
        uint32_t now = sys_now();
        const uint32_t TIMEOUT_MS = TCP_SERVER_TIMEOUT_S * 1000;
        if(now - conn->last_active_tick > TIMEOUT_MS) {
                if(conn->server->cbs->on_disconnect) {
                        conn->server->cbs->on_disconnect(conn);
                }
                
                tcp_arg(tpcb, NULL);
                tcp_close(tpcb);
                conn->pcb = NULL;
                _internal_connection_free(conn);
        }
        return ERR_OK;
}

static err_t _lwip_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
        struct tcp_server *server = (struct tcp_server *)arg;
        if(err != ERR_OK || server == NULL) 
                return ERR_VAL;
        
        osMutexAcquire(server->server_lock, osWaitForever);
        if(server->active_conection >= server->max_connection) {
                // 拒接连接
                osMutexRelease(server->server_lock);
                tcp_abort(newpcb);
                return ERR_ABRT;
        }
        
        struct tcp_connection *conn = osMemoryPoolAlloc(server->conn_pool_id, 0);
        if(!conn) {
                // 内存不够
                osMutexRelease(server->server_lock);
                return ERR_MEM;
        }
        
        memset(conn, 0, sizeof(struct tcp_connection));
        conn->pcb = newpcb;
        conn->server = server;
        conn->last_active_tick = sys_now();
        /// @TODO
        conn->connection_id = (uint32_t)newpcb;
        conn->state = 1;
        server->active_conection++;
        
        conn->next = server->conn_list;
        server->conn_list = conn;
        
        osMutexRelease(server->server_lock);
        
        // 绑定 LwIP 回调
        tcp_arg(newpcb, conn);
        tcp_poll(newpcb, _lwip_poll_cb, 4);
        tcp_recv(newpcb, _lwip_recv_cb);
        tcp_sent(newpcb, _lwip_sent_cb);
        tcp_err(newpcb, _lwip_error_cb);
        
        if(server->cbs->on_connect) {
                server->cbs->on_connect(conn);
        }
        
        return ERR_OK;
}

static void _server_start_internal(void *arg)
{
        struct tcp_server *server = (struct tcp_server *)arg;
        struct tcp_pcb *pcb = tcp_new();
        
        if(pcb) {
                if(tcp_bind(pcb, IP_ADDR_ANY, server->port) == ERR_OK) {
                        server->listen_pcb = tcp_listen(pcb);
                        tcp_arg(server->listen_pcb, server);
                        tcp_accept(server->listen_pcb, _lwip_accept_cb);
                }
        }
        
}

struct send_req {
        uint8_t payload[TCP_SERVER_SEND_REQ_BUF_SIZE];
        struct tcp_connection *conn;
        uint16_t len;
};

int tcp_server_init(struct tcp_server *server, uint16_t port, uint16_t max_conn, const struct tcp_server_cbs *cbs)
{
        if(!server || !cbs)
                return -1;
        
        server->port = port;
        server->max_connection = max_conn;
        server->active_conection = 0;
        server->cbs = cbs;
        server->conn_list = NULL;
        
        server->server_lock = osMutexNew(NULL);
        server->conn_pool_id = osMemoryPoolNew(max_conn, sizeof(struct tcp_connection), NULL);
        server->req_message_id = osMemoryPoolNew(16, sizeof(struct send_req), NULL); 
        
        if(server->server_lock == NULL  || 
           server->conn_pool_id == NULL || 
           server->req_message_id == NULL) {
                return -2;
        }
        
        tcpip_callback(_server_start_internal, server);
        return 0;
}

void static _tcp_server_close_executor(void *arg)
{
        struct tcp_server *server = (struct tcp_server *)arg;
        if(!server) 
                return;
        
        if(server->listen_pcb) {
                tcp_arg(server->listen_pcb, NULL);
                tcp_accept(server->listen_pcb, NULL);
                tcp_close(server->listen_pcb);
                server->listen_pcb = NULL;
        }
        
        struct tcp_connection *conn = server->conn_list;
        while(conn) {
                struct tcp_connection *next = conn->next;
                tcp_server_conn_close(conn);
                conn = next;
        }
        
        server->conn_list = NULL;
        server->active_conection = 0;
}

void tcp_server_close(struct tcp_server *server)
{
        if(!server)
                return;
        
        tcpip_callback(_tcp_server_close_executor, server);
        if(server->server_lock) {
                osMutexDelete(server->server_lock);
                server->server_lock = NULL;
        }
        
        if(server->conn_pool_id) {
                osMemoryPoolDelete(server->conn_pool_id);
                server->conn_pool_id = NULL;
        }
        
        if(server->req_message_id) {
                osMemoryPoolDelete(server->req_message_id);
                server->req_message_id = NULL;
        }
}

static void _safe_write_executor(void *arg)
{
    struct send_req *req = (struct send_req *)arg;
    struct tcp_connection *conn = req->conn;
    
    if(!conn || conn->state == 0 || conn->pcb == NULL) {
        osMemoryPoolFree(req->conn->server->req_message_id, req);
        return;
    }

    err_t err = tcp_write(conn->pcb, req->payload, req->len, TCP_WRITE_FLAG_COPY);
    if(err == ERR_OK) {
        tcp_output(conn->pcb);
        conn->unacked_len += req->len;
    }

    osMemoryPoolFree(conn->server->req_message_id, req);
}

static void _safe_close_executor(void *arg)
{
        struct tcp_connection *conn = (struct tcp_connection *)arg;
        if(!conn || conn->state == 0 || conn->pcb == NULL) {
                return;
        }
        
        struct tcp_pcb *pcb = conn->pcb;
        
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        
        tcp_close(pcb);
        conn->pcb = NULL;
        
        _internal_connection_free(conn);
}

void tcp_server_send(struct tcp_connection *conn, const void *data, uint16_t len)
{
        if(!conn || !data || len == 0)
                return;
        
        if(conn->state == 0 || conn->pcb == NULL)
                return;
        
        const uint8_t *pdata = (const uint8_t *)data;
        uint16_t remaining = len;
        while(remaining > 0) {
                uint16_t chunk = (remaining > TCP_SERVER_SEND_REQ_BUF_SIZE) 
                                            ? TCP_SERVER_SEND_REQ_BUF_SIZE 
                                            : remaining;
                struct send_req *req = osMemoryPoolAlloc(conn->server->req_message_id, 0);
                if(!req) {
                        // 内存满了
                        break;
                }
                req->conn = conn;
                req->len = chunk;
                memcpy(req->payload, pdata, chunk);
                if(tcpip_callback(_safe_write_executor, req) != ERR_OK) {
                        osMemoryPoolFree(conn->server->req_message_id, req);
                        break;
                }
                
                pdata += chunk;
                remaining -= chunk;
        }
}


bool tcp_server_conn_is_alive(const struct tcp_connection *conn)
{
        return conn->state == 1;
}

void tcp_server_conn_close(struct tcp_connection *conn)
{
        tcpip_callback(_safe_close_executor, conn);
}

