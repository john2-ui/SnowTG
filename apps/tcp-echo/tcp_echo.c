#include "tcp_echo.h"

#include "../../pro-stack/config.h"
#include "../../pro-stack/log.h"
#include "../../pro-stack/net_context.h"
#include "../../pro-stack/socket_api.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>

#define TCP_APP_BACKLOG 16
#define TCP_APP_RECV_BUFFER_SIZE 1280
#define TCP_CLIENT_RETRY_SEC 2
#define TCP_CLIENT_MSG "hello from tcp client\n"

int tcp_echo_server_entry(__attribute__((unused)) void *arg) {
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

        LOG_MOD_INFO("APP", "tcp-server event=listening local=" IP_FMT ":%u",
                     IP_ARG(g_net.local_ip),
                     rte_be_to_cpu_16(local_addr.sin_port));

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

                while (1) {
                        ssize_t received =
                            nrecv(conn_fd, buffer, sizeof(buffer), 0);
                        if (received <= 0)
                                break;
                        if (nsend(conn_fd, buffer, (size_t)received, 0) < 0)
                                break;
                }
                nclose(conn_fd);
        }
out:
        nclose(listen_fd);
        return ret;
}

int tcp_echo_client_entry(__attribute__((unused)) void *arg) {
        struct sockaddr_in peer_addr;
        memset(&peer_addr, 0, sizeof(peer_addr));
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(TCP_APP_PORT);
        peer_addr.sin_addr.s_addr = TCP_CLIENT_PEER_IP;

        char buffer[TCP_APP_RECV_BUFFER_SIZE];
        while (1) {
                int fd = nsocket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0)
                        goto retry;
                if (nconnect(fd, (struct sockaddr *)&peer_addr,
                             sizeof(peer_addr)) < 0) {
                        nclose(fd);
                        goto retry;
                }
                if (nsend(fd, TCP_CLIENT_MSG, strlen(TCP_CLIENT_MSG), 0) >= 0)
                        (void)nrecv(fd, buffer, sizeof(buffer), 0);
                nclose(fd);
        retry:
                sleep(TCP_CLIENT_RETRY_SEC);
        }
}
