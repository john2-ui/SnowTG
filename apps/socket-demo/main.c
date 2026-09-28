/**
 * @file main.c
 * @brief Public nonblocking socket examples on one packet owner and a pthread.
 *
 * Application code uses only integer descriptors and nepoll. The EAL main
 * lcore owns ingress, transport state and timers; it must keep running until
 * the application has closed its sockets and poller. SIGINT/SIGTERM only set
 * a flag: signal handlers must never call synchronous n* APIs.
 */
#include "../../pro-stack/arp.h"
#include "../../pro-stack/nepoll.h"
#include "../../pro-stack/net_context.h"
#include "../../pro-stack/port.h"
#include "../../pro-stack/ring.h"
#include "../../pro-stack/rx_dispatch.h"
#include "../../pro-stack/socket.h"
#include "../../pro-stack/stack_runtime.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLIENT_LIMIT 32
#define BUFFER_SIZE 4096

static volatile sig_atomic_t interrupted;
static const char *mode;
static struct sockaddr_in address;
static int application_result;

static void on_signal(int signal_number) {
        (void)signal_number;
        interrupted = 1;
}

/** Change LT interest: WRITE is requested only while bytes need sending. */
static int interest(int poller, int operation, int fd, uint32_t events,
                    uint64_t data) {
        struct nepoll_event event = {.events = events, .data = data};
        return nepoll_ctl(poller, operation, fd, &event);
}

/** Wait for a single-client operation, bounded even without packet ingress. */
static int wait_ready(int poller, int fd, uint32_t events) {
        if (interest(poller, NEPOLL_CTL_MOD, fd, events, 0) < 0)
                return -1;
        for (unsigned i = 0; i < 20 && !interrupted; i++) {
                struct nepoll_event event;
                int count = nepoll_wait(poller, &event, 1, 250);
                if (count != 0)
                        return count < 0 ? -1 : 0;
        }
        errno = interrupted ? EINTR : ETIMEDOUT;
        return -1;
}

/** Asynchronous connect, short writes/reads, and SO_ERROR consumption. */
static int tcp_client(int poller, int fd) {
        if (interest(poller, NEPOLL_CTL_ADD, fd, NEPOLL_CONNECTED, 0) < 0)
                return -1;
        if (nconnect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
                if (errno != EINPROGRESS ||
                    wait_ready(poller, fd, NEPOLL_CONNECTED) < 0)
                        return -1;
        }
        /* ERROR/HUP are delivered even when only CONNECTED was requested. */
        int error;
        socklen_t size = sizeof(error);
        if (ngetsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
                return -1;
        if (error) {
                errno = error;
                return -1;
        }
        const char message[] = "hello from nonblocking client\n";
        size_t sent = 0, received = 0;
        char reply[sizeof(message) - 1];
        while (sent < sizeof(message) - 1) {
                ssize_t n =
                    nsend(fd, message + sent, sizeof(message) - 1 - sent, 0);
                if (n > 0)
                        sent += (size_t)n;
                else if (n < 0 && errno == EAGAIN) {
                        if (wait_ready(poller, fd, NEPOLL_WRITE) < 0)
                                return -1;
                } else {
                        if (!n)
                                errno = EIO;
                        return -1;
                }
        }
        while (received < sizeof(reply)) {
                ssize_t n =
                    nrecv(fd, reply + received, sizeof(reply) - received, 0);
                if (n > 0)
                        received += (size_t)n;
                else if (n < 0 && errno == EAGAIN) {
                        if (wait_ready(poller, fd, NEPOLL_READ) < 0)
                                return -1;
                } else {
                        if (!n)
                                errno = ECONNRESET; /* EOF before full echo. */
                        return -1;
                }
        }
        if (memcmp(reply, message, sizeof(reply))) {
                errno = EPROTO;
                return -1;
        }
        printf("tcp-client: echoed %zu bytes\n", received);
        return 0;
}

struct client {
        int fd;
        size_t offset, length;
        char bytes[BUFFER_SIZE];
};

/** Release registration before fd reuse; no subscription owns the socket. */
static void client_close(int poller, struct client *client) {
        nepoll_ctl(poller, NEPOLL_CTL_DEL, client->fd, NULL);
        nclose(client->fd);
        client->fd = -1;
}

/** Bounded buffering: pause reads until the current echo has been sent. */
static int tcp_server(int poller, int listener) {
        struct client clients[CLIENT_LIMIT];
        for (unsigned i = 0; i < CLIENT_LIMIT; i++)
                clients[i].fd = -1;
        if (nlisten(listener, CLIENT_LIMIT) < 0 ||
            interest(poller, NEPOLL_CTL_ADD, listener, NEPOLL_ACCEPT,
                     CLIENT_LIMIT) < 0)
                return -1;
        int result = 0;
        while (!interrupted) {
                struct nepoll_event events[CLIENT_LIMIT + 1];
                int count = nepoll_wait(poller, events, CLIENT_LIMIT + 1, 250);
                if (count < 0) {
                        result = -1;
                        break;
                }
                for (int i = 0; i < count; i++) {
                        if (events[i].data == CLIENT_LIMIT) {
                                /* Accept at most one batch, so a connection
                                 * flood cannot starve established clients. */
                                for (unsigned j = 0; j < CLIENT_LIMIT; j++) {
                                        int fd = naccept4(listener, NULL, NULL,
                                                          SOCK_NONBLOCK);
                                        if (fd < 0) {
                                                if (errno != EAGAIN)
                                                        result = -1;
                                                break;
                                        }
                                        unsigned slot = 0;
                                        while (slot < CLIENT_LIMIT &&
                                               clients[slot].fd >= 0)
                                                slot++;
                                        if (slot == CLIENT_LIMIT) {
                                                nclose(fd);
                                                continue;
                                        }
                                        clients[slot] =
                                            (struct client){.fd = fd};
                                        if (interest(poller, NEPOLL_CTL_ADD, fd,
                                                     NEPOLL_READ, slot) < 0) {
                                                client_close(poller,
                                                             &clients[slot]);
                                                result = -1;
                                                break;
                                        }
                                }
                                continue;
                        }
                        struct client *c = &clients[events[i].data];
                        if (c->fd != events[i].fd)
                                continue;
                        /* HUP can accompany unread bytes. recv distinguishes
                         * buffered data, normal EOF and a sticky reset error.
                         */
                        if (!c->length) {
                                ssize_t n =
                                    nrecv(c->fd, c->bytes, sizeof(c->bytes), 0);
                                if (n == 0 || (n < 0 && errno != EAGAIN)) {
                                        client_close(poller, c);
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
                                if (n < 0 && errno != EAGAIN) {
                                        client_close(poller, c);
                                        continue;
                                }
                                if (n > 0)
                                        c->offset += (size_t)n;
                                if (c->offset == c->length)
                                        c->length = 0;
                        }
                        if (interest(poller, NEPOLL_CTL_MOD, c->fd,
                                     c->length ? NEPOLL_WRITE : NEPOLL_READ,
                                     events[i].data) < 0) {
                                result = -1;
                                break;
                        }
                }
                if (result < 0)
                        break;
        }
        int error = errno;
        for (unsigned i = 0; i < CLIENT_LIMIT; i++)
                if (clients[i].fd >= 0)
                        client_close(poller, &clients[i]);
        errno = error;
        return result;
}

/** Preserve one datagram while sendto is backpressured, including empty ones.
 */
static int udp_server(int poller, int fd) {
        unsigned char bytes[65507];
        struct sockaddr_in peer;
        if (interest(poller, NEPOLL_CTL_ADD, fd, NEPOLL_READ, 0) < 0)
                return -1;
        while (!interrupted) {
                struct nepoll_event event;
                int count = nepoll_wait(poller, &event, 1, 250);
                if (count < 0)
                        return -1;
                if (!count)
                        continue;
                socklen_t size = sizeof(peer);
                ssize_t n = nrecvfrom(fd, bytes, sizeof(bytes), 0,
                                      (struct sockaddr *)&peer, &size);
                if (n < 0) {
                        if (errno == EAGAIN)
                                continue;
                        return -1;
                }
                for (;;) {
                        ssize_t sent = nsendto(fd, bytes, (size_t)n, 0,
                                               (struct sockaddr *)&peer, size);
                        if (sent == n)
                                break;
                        if (sent < 0 && errno == EMSGSIZE) {
                                /* Public UDP TX does not fragment. */
                                fprintf(stderr,
                                        "udp-echo: drop oversize reply\n");
                                break;
                        }
                        if (sent >= 0 || errno != EAGAIN ||
                            wait_ready(poller, fd, NEPOLL_WRITE) < 0)
                                return -1;
                }
                if (interest(poller, NEPOLL_CTL_MOD, fd, NEPOLL_READ, 0) < 0)
                        return -1;
        }
        return 0;
}

static void *application(void *unused) {
        (void)unused;
        int type = !strcmp(mode, "udp-echo") ? SOCK_DGRAM : SOCK_STREAM;
        int fd = nsocket(AF_INET, type | SOCK_NONBLOCK, 0);
        int poller = nepoll_create();
        int result = -1;
        if (fd < 0 || poller < 0)
                goto out;
        if (!strcmp(mode, "tcp-client")) {
                result = tcp_client(poller, fd);
        } else {
                int one = 1;
                if (nsetsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one,
                                sizeof(one)) < 0 ||
                    nbind(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
                        goto out;
                printf("%s: port=%u (Ctrl-C to stop)\n", mode,
                       ntohs(address.sin_port));
                fflush(stdout);
                result = type == SOCK_STREAM ? tcp_server(poller, fd)
                                             : udp_server(poller, fd);
        }
out:
        if (result < 0 && !interrupted)
                perror(mode);
        /* Keep the owner alive through all synchronous cleanup operations. */
        if (fd >= 0)
                nclose(fd);
        if (poller >= 0)
                nepoll_close(poller);
        application_result = interrupted ? 0 : result < 0;
        stack_runtime_request_stop();
        return NULL;
}

int main(int argc, char **argv) {
        int used = rte_eal_init(argc, argv);
        if (used < 0)
                rte_exit(EXIT_FAILURE, "EAL initialization failed\n");
        argc -= used;
        argv += used;
        struct in_addr local;
        char *end = NULL;
        unsigned long port = 0;
        if (argc >= 4) {
                errno = 0;
                port = strtoul(argv[3], &end, 10);
        }
        if ((argc != 4 && argc != 5) ||
            (strcmp(argv[1], "tcp-echo") && strcmp(argv[1], "udp-echo") &&
             strcmp(argv[1], "tcp-client")) ||
            (!strcmp(argv[1], "tcp-client") != (argc == 5)) ||
            inet_pton(AF_INET, argv[2], &local) != 1 || !local.s_addr || !end ||
            end == argv[3] || *end || errno || !port || port > 65535)
                rte_exit(
                    EXIT_FAILURE,
                    "usage: EAL_ARGS -- tcp-echo|udp-echo LOCAL_IP PORT\n"
                    "       EAL_ARGS -- tcp-client LOCAL_IP PORT PEER_IP\n");
        mode = argv[1];
        address = (struct sockaddr_in){.sin_family = AF_INET,
                                       .sin_port = htons((uint16_t)port),
                                       .sin_addr.s_addr = INADDR_ANY};
        if (argc == 5 && inet_pton(AF_INET, argv[4], &address.sin_addr) != 1)
                rte_exit(EXIT_FAILURE, "invalid peer IPv4 address\n");
        struct sigaction action = {.sa_handler = on_signal};
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
        struct rte_mempool *mp =
            rte_pktmbuf_pool_create("socket_demo", 16383, 0, 0,
                                    RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (!mp)
                rte_exit(EXIT_FAILURE, "mbuf pool allocation failed\n");
        net_context_set_mempool(mp);
        struct port_topology topology = port_init(0, mp, 1500);
        net_context_init(0, local.s_addr, topology.ipv4_mtu);
        unsigned owner = rte_lcore_id();
        if (owner_timer_global_init() || socket_registry_init() ||
            socket_owner_init(owner) || ring_init_owner(owner) ||
            arp_table_init_owner(owner) ||
            rx_dispatch_configure_workers(&owner, 1))
                rte_exit(EXIT_FAILURE, "owner initialization failed\n");
        struct stack_runtime_worker worker;
        if (stack_runtime_worker_init(&worker, owner, 0,
                                      NSOCK_ID_DEFAULT_CAPACITY, mp,
                                      ring_instance(), NULL, NULL))
                rte_exit(EXIT_FAILURE, "runtime initialization failed\n");
        worker.direct_rx_enabled = worker.direct_tx_enabled = true;
        worker.port_id = worker.rx_queue_id = worker.tx_queue_id = 0;
        pthread_t thread;
        if (pthread_create(&thread, NULL, application, NULL))
                rte_exit(EXIT_FAILURE, "application thread creation failed\n");
        stack_runtime_worker_entry(&worker);
        pthread_join(thread, NULL);
        rte_eth_dev_stop(0);
        rte_eth_dev_close(0);
        socket_owner_fini();
        socket_registry_fini();
        arp_table_fini();
        ring_fini();
        rte_mempool_free(mp);
        rte_eal_cleanup();
        return application_result;
}
