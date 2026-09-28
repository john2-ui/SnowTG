/**
 * @file udp_echo.c
 * @brief Blocking UDP echo with one receive buffer per complete datagram.
 */
#include "udp_echo.h"

#include "../../pro-stack/log.h"
#include "../../pro-stack/net_context.h"
#include "../../pro-stack/socket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>

#define UDP_APP_PORT 8889
/* The stack retains unread UDP suffixes: a small buffer would turn one
 * incoming datagram into multiple echo replies. TX still enforces the MTU. */
#define UDP_APP_RECV_BUFFER_SIZE 65507

int udp_echo_entry(__attribute__((unused)) void *arg) {
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
                nclose(socket_fd);
                return -1;
        }

        char buffer[UDP_APP_RECV_BUFFER_SIZE];
        while (1) {
                struct sockaddr_in client_addr;
                socklen_t client_addr_len = sizeof(client_addr);
                ssize_t received = nrecvfrom(socket_fd, buffer, sizeof(buffer),
                                             0, (struct sockaddr *)&client_addr,
                                             &client_addr_len);
                if (received < 0)
                        continue;
                if (nsendto(socket_fd, buffer, (size_t)received, 0,
                            (struct sockaddr *)&client_addr,
                            sizeof(client_addr)) < 0)
                        LOG_ERROR("nsendto failed");
        }
}
