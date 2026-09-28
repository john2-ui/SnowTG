/**
 * @file tcp_echo.c
 * @brief Concurrent nonblocking TCP echo server and blocking echo client.
 */
#include "tcp_echo.h"

#include "../../pro-stack/config.h"
#include "../../pro-stack/log.h"
#include "../../pro-stack/nepoll.h"
#include "../../pro-stack/net_context.h"
#include "../../pro-stack/socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>

#define TCP_APP_BACKLOG 16
#define TCP_APP_CLIENT_LIMIT 32
#define TCP_APP_RECV_BUFFER_SIZE 1280
#define TCP_CLIENT_RETRY_SEC 2
#define TCP_CLIENT_MSG "hello from tcp client\n"

/** Blocking send still permits short writes; advance by the returned count. */
static int send_all(int fd, const void *data, size_t length) {
        const char *bytes = data;
        size_t offset = 0;
        while (offset < length) {
                ssize_t n = nsend(fd, bytes + offset, length - offset, 0);
                if (n <= 0)
                        return -1;
                offset += (size_t)n;
        }
        return 0;
}

struct echo_client {
        int fd;
        uint32_t generation;
        size_t offset, length;
        char bytes[TCP_APP_RECV_BUFFER_SIZE];
};

static int server_interest(int poller, int operation, int fd, uint32_t events,
                           uint64_t token) {
        struct nepoll_event event = {.events = events, .data = token};
        return nepoll_ctl(poller, operation, fd, &event);
}

static void server_client_close(int poller, struct echo_client *client) {
        nepoll_ctl(poller, NEPOLL_CTL_DEL, client->fd, NULL);
        nclose(client->fd);
        client->fd = -1;
}

int tcp_echo_server_entry(__attribute__((unused)) void *arg) {
        struct echo_client clients[TCP_APP_CLIENT_LIMIT] = {0};
        for (unsigned i = 0; i < TCP_APP_CLIENT_LIMIT; i++)
                clients[i].fd = -1;
        int poller = -1, error;
        int listen_fd = nsocket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (listen_fd < 0)
                goto out;
        poller = nepoll_create();
        if (poller < 0)
                goto out;

        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(TCP_APP_PORT);
        local_addr.sin_addr.s_addr = g_net.local_ip;

        if (nbind(listen_fd, (struct sockaddr *)&local_addr,
                  sizeof(local_addr)) < 0)
                goto out;
        if (nlisten(listen_fd, TCP_APP_BACKLOG) < 0 ||
            server_interest(poller, NEPOLL_CTL_ADD, listen_fd, NEPOLL_ACCEPT,
                            UINT64_MAX) < 0)
                goto out;

        LOG_MOD_INFO("APP", "tcp-server event=listening local=" IP_FMT ":%u",
                     IP_ARG(g_net.local_ip),
                     rte_be_to_cpu_16(local_addr.sin_port));

        while (1) {
                struct nepoll_event events[TCP_APP_CLIENT_LIMIT + 1];
                int count =
                    nepoll_wait(poller, events, TCP_APP_CLIENT_LIMIT + 1, -1);
                if (count < 0) {
                        if (errno == EINTR)
                                continue;
                        goto out;
                }
                unsigned accepted = 0;
                for (int i = 0; i < count; i++) {
                        if (events[i].data == UINT64_MAX) {
                                if (events[i].events &
                                    (NEPOLL_ERROR | NEPOLL_HUP)) {
                                        errno = EIO;
                                        goto out;
                                }
                                /* Bound accept work so floods cannot starve
                                 * connections already in this event batch. */
                                while (accepted < TCP_APP_CLIENT_LIMIT) {
                                        accepted++;
                                        int fd = naccept4(listen_fd, NULL, NULL,
                                                          SOCK_NONBLOCK);
                                        if (fd < 0) {
                                                if (errno == EAGAIN)
                                                        break;
                                                if (errno == EINTR ||
                                                    errno == ECONNABORTED)
                                                        continue;
                                                goto out;
                                        }
                                        unsigned slot = 0;
                                        while (slot < TCP_APP_CLIENT_LIMIT &&
                                               clients[slot].fd >= 0)
                                                slot++;
                                        if (slot == TCP_APP_CLIENT_LIMIT) {
                                                nclose(fd);
                                                continue;
                                        }
                                        struct echo_client *c = &clients[slot];
                                        c->fd = fd;
                                        c->generation++;
                                        c->offset = c->length = 0;
                                        uint64_t token =
                                            ((uint64_t)c->generation << 32) |
                                            slot;
                                        if (server_interest(
                                                poller, NEPOLL_CTL_ADD, fd,
                                                NEPOLL_READ, token) < 0)
                                                goto out;
                                }
                                continue;
                        }
                        unsigned slot = (uint32_t)events[i].data;
                        if (slot >= TCP_APP_CLIENT_LIMIT)
                                continue;
                        struct echo_client *c = &clients[slot];
                        if (c->fd < 0 || c->fd != events[i].fd ||
                            c->generation != (events[i].data >> 32))
                                continue;
                        /* Keep one bounded echo buffer. HUP can accompany
                         * unread data; recv, not the event, establishes EOF. */
                        if (!c->length) {
                                ssize_t n =
                                    nrecv(c->fd, c->bytes, sizeof(c->bytes), 0);
                                if (!n || (n < 0 && errno != EAGAIN &&
                                           errno != EINTR)) {
                                        server_client_close(poller, c);
                                        continue;
                                }
                                if (n > 0) {
                                        c->length = (size_t)n;
                                        c->offset = 0;
                                }
                        }
                        if (c->length) {
                                ssize_t n = nsend(c->fd, c->bytes + c->offset,
                                                  c->length - c->offset, 0);
                                if (!n || (n < 0 && errno != EAGAIN &&
                                           errno != EINTR)) {
                                        server_client_close(poller, c);
                                        continue;
                                }
                                if (n > 0)
                                        c->offset += (size_t)n;
                                if (c->offset == c->length)
                                        c->length = 0;
                        }
                        if (server_interest(poller, NEPOLL_CTL_MOD, c->fd,
                                            c->length ? NEPOLL_WRITE
                                                      : NEPOLL_READ,
                                            events[i].data) < 0)
                                goto out;
                }
        }
out:
        error = errno;
        LOG_ERROR("tcp_server: event loop failed: %s", strerror(error));
        for (unsigned i = 0; i < TCP_APP_CLIENT_LIMIT; i++)
                if (clients[i].fd >= 0)
                        server_client_close(poller, &clients[i]);
        if (listen_fd >= 0) {
                if (poller >= 0)
                        nepoll_ctl(poller, NEPOLL_CTL_DEL, listen_fd, NULL);
                nclose(listen_fd);
        }
        if (poller >= 0)
                nepoll_close(poller);
        errno = error;
        return -1;
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
                if (send_all(fd, TCP_CLIENT_MSG, strlen(TCP_CLIENT_MSG)) == 0) {
                        size_t remaining = strlen(TCP_CLIENT_MSG);
                        while (remaining) {
                                ssize_t n = nrecv(fd, buffer, remaining, 0);
                                if (n <= 0)
                                        break;
                                remaining -= (size_t)n;
                        }
                }
                nclose(fd);
        retry:
                sleep(TCP_CLIENT_RETRY_SEC);
        }
}
