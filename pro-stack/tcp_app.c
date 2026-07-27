/**
 * @file tcp_app.c
 * @brief Example TCP echo server on the userspace stream socket API.
 *
 * Exercises nlisten/naccept plus nrecv/nsend on the accepted connection.
 */
#include "tcp_app.h"

#include "log.h"
#include "net_context.h"
#include "socket_api.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>

#define TCP_APP_PORT 8888
#define TCP_APP_BACKLOG 16
#define TCP_APP_RECV_BUFFER_SIZE 128

int tcp_app_entry(__attribute__((unused)) void *arg) {
        int ret = 0;
        int listen_fd = nsocket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
                LOG_ERROR("tcp_app: nsocket failed");
                return -1;
        }

        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(TCP_APP_PORT);
        local_addr.sin_addr.s_addr = g_net.local_ip;

        if (nbind(listen_fd, (struct sockaddr *)&local_addr,
                  sizeof(local_addr)) < 0) {
                LOG_ERROR("tcp_app: nbind failed");
                ret = -1;
                goto out;
        }

        if (nlisten(listen_fd, TCP_APP_BACKLOG) < 0) {
                LOG_ERROR("tcp_app: nlisten failed");
                ret = -1;
                goto out;
        }

        LOG_INFO("TCP server listening on " IP_FMT ":%u",
                 IP_ARG(g_net.local_ip), rte_be_to_cpu_16(local_addr.sin_port));

        char buffer[TCP_APP_RECV_BUFFER_SIZE];

        while (1) {
                struct sockaddr_in peer_addr;
                socklen_t peer_len = sizeof(peer_addr);
                int conn_fd = naccept(listen_fd, (struct sockaddr *)&peer_addr,
                                      &peer_len);
                if (conn_fd < 0) {
                        LOG_ERROR("tcp_app: naccept failed");
                        continue;
                }

                LOG_INFO("tcp_app accepted fd=%d peer " IP_FMT ":%u", conn_fd,
                         IP_ARG(peer_addr.sin_addr.s_addr),
                         rte_be_to_cpu_16(peer_addr.sin_port));

                while (1) {
                        ssize_t received =
                            nrecv(conn_fd, buffer, sizeof(buffer), 0);
                        if (received <= 0)
                                break;

                        LOG_INFO("tcp app recv fd=%d len=%zd data=%.*s",
                                 conn_fd, received, (int)received, buffer);

                        if (nsend(conn_fd, buffer, (size_t)received, 0) < 0) {
                                LOG_ERROR("tcp_app: nsend failed fd=%d",
                                          conn_fd);
                                break;
                        }
                }

                nclose(conn_fd);
                LOG_INFO("tcp_app closed fd=%d", conn_fd);
        }

out:
        nclose(listen_fd);
        return ret;
}
