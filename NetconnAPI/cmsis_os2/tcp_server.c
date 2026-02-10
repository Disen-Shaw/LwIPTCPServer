
#include "tcp_server.h"
#include <string.h>

static void tcp_worker_thread(void *args);
static void tcp_monitor_thread(void *args);

err_t tcp_server_init(struct tcp_server *server, uint16_t port, uint16_t max_conns, 
                      const struct tcp_server_cbs *cbs)
{
        if(server == NULL) {
                return ERR_VAL;
        }
        
        memset(server, 0, sizeof(struct tcp_server));
        server->port = port;
        server->max_conns = max_conns;
        server->cbs = cbs;
        
        // 互斥锁用于保护 active_conns 计数
        server->list_mutex = osMutexNew(NULL);
        if(server->list_mutex == NULL) {
                return ERR_MEM;
        }
        
        return ERR_OK;
}

err_t tcp_server_start(struct tcp_server *server)
{
        osThreadAttr_t attr = {
                .name = "tcp_listen",
                .stack_size = 2048,
                .priority = osPriorityNormal,
        };
        
        server->server_monitor = osThreadNew(tcp_monitor_thread, server, &attr);
        return (server->server_monitor != NULL) ? ERR_OK : ERR_MEM;
}

err_t tcp_conn_send(struct tcp_conn *conn, const void *data, size_t len)
{
        if(!conn || !conn->conn ||conn->is_closing)
                return ERR_CONN;
        
        return netconn_write(conn->conn, data, len, NETCONN_COPY);
}

static void tcp_monitor_thread(void *args)
{
    struct tcp_server *server = (struct tcp_server *)args;
    struct netconn *new_conn = NULL;

    // 创建监听 socket
    server->listen_conn = netconn_new(NETCONN_TCP);
    netconn_bind(server->listen_conn, IP_ADDR_ANY, server->port);
    netconn_listen(server->listen_conn);

    // 监听 socket 设置为非阻塞
    netconn_set_nonblocking(server->listen_conn, 1);

    for (;;) {

        // 非阻塞 accept
        err_t err = netconn_accept(server->listen_conn, &new_conn);

        if (err == ERR_OK) {

            // 在 accept 之后立即检查连接数
            osMutexAcquire(server->list_mutex, osWaitForever);
            bool allow = (server->active_conns < server->max_conns);
            osMutexRelease(server->list_mutex);

            if (!allow) {
                // 直接关闭，不创建 worker，不占用 backlog
                netconn_close(new_conn);
                netconn_delete(new_conn);
                continue;
            }

            // 创建连接上下文
            struct tcp_conn *conn = mem_malloc(sizeof(struct tcp_conn));
            if (!conn) {
                netconn_close(new_conn);
                netconn_delete(new_conn);
                continue;
            }

            memset(conn, 0, sizeof(*conn));
            conn->conn   = new_conn;
            conn->server = server;
            conn->state  = TCP_CONN_STATE_INIT;
            conn->last_active_tick = sys_now();

            // ? 创建 worker 线程
            osThreadAttr_t attr = {
                .stack_size = 2048,
                .priority   = osPriorityNormal,
            };
            conn->thread_id = osThreadNew(tcp_worker_thread, conn, &attr);

            if (conn->thread_id == NULL) {
                netconn_close(new_conn);
                netconn_delete(new_conn);
                mem_free(conn);
            }
        }
        else if (err == ERR_WOULDBLOCK) {
            osDelay(10);
        }
        else {
            // 其他错误，稍等再试
            osDelay(10);
        }
    }
}


static void tcp_worker_thread(void *args)
{
        struct tcp_conn *conn = (struct tcp_conn *)args;
        struct tcp_server *server = conn->server;
        struct netbuf *buf;

        // 先做接入控制
        osMutexAcquire(server->list_mutex, osWaitForever);

        if (server->active_conns >= server->max_conns) {
                osMutexRelease(server->list_mutex);

                // 服务器忙，礼貌地拒绝（可选）
                const char *busy_msg = "Server busy\r\n";
                netconn_write(conn->conn, busy_msg, strlen(busy_msg), NETCONN_COPY);

                netconn_close(conn->conn);
                netconn_delete(conn->conn);
                conn->state = TCP_CONN_STATE_CLOSED;
                mem_free(conn);
                osThreadExit();
        }

        server->active_conns++;
        conn->state = TCP_CONN_STATE_ACTIVE;
        osMutexRelease(server->list_mutex);

        // 回调 on_connect
        if (server->cbs->on_connect) {
                server->cbs->on_connect(conn);
        }

        netconn_set_recvtimeout(conn->conn, 1000);

        while(!conn->is_closing) {
                err_t err = netconn_recv(conn->conn, &buf);
                if(err == ERR_OK) {
                        conn->last_active_tick = sys_now();
                        if(server->cbs->on_data) {
                                server->cbs->on_data(conn, buf);
                        }
                        netbuf_delete(buf);
                } else if (err == ERR_TIMEOUT) {
                        if(sys_now() - conn->last_active_tick > TCP_SERVER_ACTIVE_TIMEOUT_S * 1000) {
                                break;
                        }
                        continue;
                } else {
                        break;
                }
        }

        conn->state = TCP_CONN_STATE_CLOSING;

        if(server->cbs->on_close) {
                server->cbs->on_close(conn);
        }

        netconn_close(conn->conn);
        netconn_delete(conn->conn);

        osMutexAcquire(server->list_mutex, osWaitForever);
        if (server->active_conns > 0) {
                server->active_conns--;
        }
        osMutexRelease(server->list_mutex);

        conn->state = TCP_CONN_STATE_CLOSED;
        mem_free(conn);
        osThreadExit();
}
