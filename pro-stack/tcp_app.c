/**
 * @file tcp_app.c
 * @brief Example TCP server and client on the userspace stream socket API.
 *
 * Server path (passive open): nlisten / naccept / nrecv / nsend / nclose.
 * Client path (active open):  nconnect / nsend / nrecv / nclose (no nbind;
 * tcp_connect performs implicit bind / ephemeral port allocation).
 *
 * Toggle ENABLE_TCP_SERVER / ENABLE_TCP_CLIENT in config.h.
 */
#include "tcp_app.h"

#include "config.h"
#include "log.h"
#include "net_context.h"
#include "socket_api.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>

#define TCP_APP_BACKLOG 16
#define TCP_APP_RECV_BUFFER_SIZE 1280
#define TCP_CLIENT_RETRY_SEC 2
#define TCP_CLIENT_MSG "hello from tcp client\n"

int tcp_server_entry(__attribute__((unused)) void *arg) {
        int ret = 0;
        int listen_fd = nsocket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
                LOG_ERROR("tcp_server: nsocket failed");
                return -1;
        }

        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(TCP_APP_PORT);
        local_addr.sin_addr.s_addr = g_net.local_ip;

        if (nbind(listen_fd, (struct sockaddr *)&local_addr,
                  sizeof(local_addr)) < 0) {
                LOG_ERROR("tcp_server: nbind failed");
                ret = -1;
                goto out;
        }

        if (nlisten(listen_fd, TCP_APP_BACKLOG) < 0) {
                LOG_ERROR("tcp_server: nlisten failed");
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
                        LOG_ERROR("tcp_server: naccept failed");
                        continue;
                }

                LOG_INFO("tcp_server accepted fd=%d peer " IP_FMT ":%u",
                         conn_fd, IP_ARG(peer_addr.sin_addr.s_addr),
                         rte_be_to_cpu_16(peer_addr.sin_port));

                while (1) {
                        ssize_t received =
                            nrecv(conn_fd, buffer, sizeof(buffer), 0);
                        if (received <= 0)
                                break;

                        LOG_INFO("tcp_server recv fd=%d len=%zd data=%.*s",
                                 conn_fd, received, (int)received, buffer);

                        if (nsend(conn_fd, buffer, (size_t)received, 0) < 0) {
                                LOG_ERROR("tcp_server: nsend failed fd=%d",
                                          conn_fd);
                                break;
                        }
                }

                nclose(conn_fd);
                LOG_INFO("tcp_server closed fd=%d", conn_fd);
        }

out:
        nclose(listen_fd);
        return ret;
}

int tcp_client_entry(__attribute__((unused)) void *arg) {
        /*
         * Stack checklist this app exercises:
         *   1) nconnect: send SYN, enter SYN_SENT, wait for SYN+ACK, send ACK,
         *      enter ESTABLISHED (block until done or fail).
         *   2) nsend/nrecv on the established socket.
         *   3) nclose from ESTABLISHED: active close (FIN_WAIT_* / TIME_WAIT).
         */
        struct sockaddr_in peer_addr;
        memset(&peer_addr, 0, sizeof(peer_addr));
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(TCP_APP_PORT);
        peer_addr.sin_addr.s_addr = TCP_CLIENT_PEER_IP;

        LOG_INFO("TCP client targeting " IP_FMT ":%u (implicit local bind)",
                 IP_ARG(TCP_CLIENT_PEER_IP), TCP_APP_PORT);

        char buffer[TCP_APP_RECV_BUFFER_SIZE];

        while (1) {
                int fd = nsocket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) {
                        LOG_ERROR("tcp_client: nsocket failed");
                        sleep(TCP_CLIENT_RETRY_SEC);
                        continue;
                }

                if (nconnect(fd, (struct sockaddr *)&peer_addr,
                             sizeof(peer_addr)) < 0) {
                        LOG_ERROR("tcp_client: nconnect failed");
                        nclose(fd);
                        sleep(TCP_CLIENT_RETRY_SEC);
                        continue;
                }

                LOG_INFO("tcp_client connected fd=%d -> " IP_FMT ":%u", fd,
                         IP_ARG(peer_addr.sin_addr.s_addr),
                         rte_be_to_cpu_16(peer_addr.sin_port));

                const size_t msg_len = strlen(TCP_CLIENT_MSG);
                if (nsend(fd, TCP_CLIENT_MSG, msg_len, 0) < 0) {
                        LOG_ERROR("tcp_client: nsend failed fd=%d", fd);
                        nclose(fd);
                        sleep(TCP_CLIENT_RETRY_SEC);
                        continue;
                }

                ssize_t received = nrecv(fd, buffer, sizeof(buffer), 0);
                if (received > 0) {
                        LOG_INFO("tcp_client recv fd=%d len=%zd data=%.*s", fd,
                                 received, (int)received, buffer);
                } else {
                        LOG_ERROR("tcp_client: nrecv failed fd=%d", fd);
                }

                nclose(fd);
                LOG_INFO("tcp_client closed fd=%d; retry in %ds", fd,
                         TCP_CLIENT_RETRY_SEC);
                sleep(TCP_CLIENT_RETRY_SEC);
        }

        return 0;
}
