/**
 * @file udp_app.c
 * @brief Example application built on the userspace datagram socket API.
 *
 * The UDP socket is only the local 2-tuple; each recvfrom/sendto carries the
 * peer address for that datagram.
 */
#include "udp_app.h"

#include "log.h"
#include "net_context.h"
#include "socket_api.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>

#define UDP_APP_PORT 8889
#define UDP_APP_RECV_BUFFER_SIZE 128

int udp_app_entry(__attribute__((unused)) void *arg) {
        int socket_fd = nsocket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd < 0) {
                LOG_ERROR("nsocket failed");
                return -1;
        }

        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(UDP_APP_PORT);
        local_addr.sin_addr.s_addr = g_net.local_ip;

        if (nbind(socket_fd, (struct sockaddr *)&local_addr,
                  sizeof(local_addr)) < 0) {
                LOG_ERROR("nbind failed");
                return -1;
        }

        LOG_INFO("UDP server listening on " IP_FMT ":%u",
                 IP_ARG(g_net.local_ip), rte_be_to_cpu_16(local_addr.sin_port));

        char buffer[UDP_APP_RECV_BUFFER_SIZE] = {0};
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        while (1) {
                ssize_t received = nrecvfrom(socket_fd, buffer, sizeof(buffer),
                                             0, (struct sockaddr *)&client_addr,
                                             &client_addr_len);
                if (received < 0)
                        continue;

                LOG_INFO("udp app recv from " IP_FMT ":%u len=%zd data=%.*s",
                         IP_ARG(client_addr.sin_addr.s_addr),
                         rte_be_to_cpu_16(client_addr.sin_port), received,
                         (int)received, buffer);

                if (nsendto(socket_fd, buffer, (size_t)received, 0,
                            (struct sockaddr *)&client_addr,
                            sizeof(client_addr)) < 0)
                        LOG_ERROR("nsendto failed");
        }

        return 0;
}
