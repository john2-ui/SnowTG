/**
 * @file test_socket_live.c
 * @brief Real-peer acceptance of the public nonblocking TCP/UDP socket API.
 *
 * Run with a prepared DPDK port (AF_PACKET is sufficient), then pass local IP,
 * peer IP, HTTP port and DNS port after '--'. The application is a pthread;
 * the EAL main lcore owns all transport objects and polls the actual port.
 */
#include "../pro-stack/arp.h"
#include "../pro-stack/nepoll.h"
#include "../pro-stack/net_context.h"
#include "../pro-stack/port.h"
#include "../pro-stack/ring.h"
#include "../pro-stack/rx_dispatch.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/stack_runtime.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct sockaddr_in http_peer, dns_peer;
static unsigned tcp_ok, udp_ok, accept_ok;

/** Wait on readiness, then let the actual operation determine success/error. */
static void ready(int poller, int fd, uint32_t interest) {
        struct nepoll_event event = {.events = interest};
        assert(nepoll_ctl(poller, NEPOLL_CTL_MOD, fd, &event) == 0);
        int count = nepoll_wait(poller, &event, 1, 5000);
        if (count != 1)
                fprintf(stderr,
                        "ready fd=%d interest=%u result=%d errno=%d "
                        "tcp=%u udp=%u accept=%u\n",
                        fd, interest, count, errno, tcp_ok, udp_ok, accept_ok);
        assert(count == 1);
        assert(event.fd == fd);
}
static int watch(int fd) {
        int poller = nepoll_create();
        struct nepoll_event event = {.events = NEPOLL_READ | NEPOLL_WRITE};
        assert(poller > 0 &&
               nepoll_ctl(poller, NEPOLL_CTL_ADD, fd, &event) == 0);
        return poller;
}
static void *application(void *unused) {
        (void)unused;
        for (unsigned i = 0; i < 100; i++) {
                int fd = nsocket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
                assert(fd >= 0);
                int poller = watch(fd);
                assert(nconnect(fd, (struct sockaddr *)&http_peer,
                                sizeof(http_peer)) == -1 &&
                       errno == EINPROGRESS);
                ready(poller, fd, NEPOLL_CONNECTED);
                int error = -1;
                socklen_t size = sizeof(error);
                assert(ngetsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) ==
                           0 &&
                       error == 0);
                int nodelay = i % 2;
                assert(nsetsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                                   sizeof(nodelay)) == 0);
                const char request[] =
                    "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
                size_t offset = 0;
                while (offset < sizeof(request) - 1) {
                        ssize_t n = nsend(fd, request + offset,
                                          sizeof(request) - 1 - offset, 0);
                        if (n < 0) {
                                assert(errno == EAGAIN);
                                ready(poller, fd, NEPOLL_WRITE);
                        } else {
                                assert(n > 0);
                                offset += n;
                        }
                }
                char response[8192];
                offset = 0;
                for (;;) {
                        ssize_t n = nrecv(fd, response + offset,
                                          sizeof(response) - 1 - offset, 0);
                        if (n < 0) {
                                assert(errno == EAGAIN);
                                ready(poller, fd, NEPOLL_READ);
                        } else if (!n)
                                break;
                        else {
                                offset += n;
                                assert(offset < sizeof(response) - 1);
                        }
                }
                response[offset] = 0;
                assert(strstr(response, "HTTP/1.1 200") ||
                       strstr(response, "HTTP/1.0 200"));
                assert(nclose(fd) == 0 && nepoll_close(poller) == 0);
                tcp_ok++;
        }
        int fd = nsocket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        assert(fd >= 0);
        struct sockaddr_in local = {.sin_family = AF_INET,
                                    .sin_port = htons(21987),
                                    .sin_addr.s_addr = INADDR_ANY};
        assert(nbind(fd, (struct sockaddr *)&local, sizeof(local)) == 0);
        int poller = watch(fd);
        unsigned char query[] = {
            0x12, 0x34, 1,   0,   0, 1,   0,   0,   0,   0, 0, 0, 6, 's', 'n',
            'o',  'w',  't', 'g', 4, 't', 'e', 's', 't', 0, 0, 1, 0, 1};
        for (unsigned i = 0; i < 100; i++) {
                query[1] = i;
                ssize_t n;
                while ((n = nsendto(fd, query, sizeof(query), 0,
                                    (struct sockaddr *)&dns_peer,
                                    sizeof(dns_peer))) < 0) {
                        assert(errno == EAGAIN);
                        ready(poller, fd, NEPOLL_WRITE);
                }
                assert(n == sizeof(query));
                unsigned char response[512];
                struct sockaddr_in source;
                socklen_t source_len = sizeof(source);
                while ((n = nrecvfrom(fd, response, sizeof(response), 0,
                                      (struct sockaddr *)&source,
                                      &source_len)) < 0) {
                        assert(errno == EAGAIN);
                        ready(poller, fd, NEPOLL_READ);
                }
                assert(n >= 12 && response[0] == query[0] &&
                       response[1] == query[1] && (response[2] & 0x80) &&
                       !(response[3] & 0x0f));
                assert(source_len == sizeof(source) &&
                       source.sin_addr.s_addr == dns_peer.sin_addr.s_addr &&
                       source.sin_port == dns_peer.sin_port);
                udp_ok++;
        }
        assert(nclose(fd) == 0 && nepoll_close(poller) == 0);
        /* The peer initiates these connections: exercise listener readiness
         * and child nonblocking flags through the public server interface. */
        int listener = nsocket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        assert(listener >= 0);
        int one = 1;
        assert(nsetsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one,
                           sizeof(one)) == 0);
        local.sin_port = htons(21988);
        assert(nbind(listener, (struct sockaddr *)&local, sizeof(local)) == 0);
        assert(nlisten(listener, 16) == 0);
        poller = watch(listener);
        for (unsigned i = 0; i < 20; i++) {
                int child;
                while ((child = naccept4(listener, NULL, NULL, SOCK_NONBLOCK)) <
                       0) {
                        assert(errno == EAGAIN);
                        ready(poller, listener, NEPOLL_ACCEPT);
                }
                assert(nfcntl(child, F_GETFL) & O_NONBLOCK);
                int child_poller = watch(child);
                char bytes[4];
                size_t offset = 0;
                while (offset < sizeof(bytes)) {
                        ssize_t n = nrecv(child, bytes + offset,
                                          sizeof(bytes) - offset, 0);
                        if (n < 0) {
                                assert(errno == EAGAIN);
                                ready(child_poller, child, NEPOLL_READ);
                        } else {
                                assert(n > 0);
                                offset += n;
                        }
                }
                assert(!memcmp(bytes, "ping", 4));
                assert(nsend(child, "pong", 4, 0) == 4);
                assert(nclose(child) == 0 && nepoll_close(child_poller) == 0);
                accept_ok++;
        }
        assert(nclose(listener) == 0 && nepoll_close(poller) == 0);

        /* A refused handshake reports one consumable SO_ERROR, while transport
         * terminal error remains available to subsequent failed operations. */
        fd = nsocket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        assert(fd >= 0);
        poller = watch(fd);
        struct sockaddr_in refused = http_peer;
        refused.sin_port = htons(21999);
        assert(nconnect(fd, (struct sockaddr *)&refused, sizeof(refused)) ==
                   -1 &&
               errno == EINPROGRESS);
        ready(poller, fd, NEPOLL_CONNECTED);
        int error = 0;
        socklen_t size = sizeof(error);
        assert(ngetsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
               error != 0);
        int original_error = error;
        assert(ngetsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
               error == 0);
        char byte;
        assert(nrecv(fd, &byte, 1, 0) == -1 && errno == original_error);
        assert(nclose(fd) == 0 && nepoll_close(poller) == 0);
        stack_runtime_request_stop();
        return NULL;
}
int main(int argc, char **argv) {
        alarm(120);
        int used = rte_eal_init(argc, argv);
        assert(used >= 0);
        argc -= used;
        argv += used;
        if (argc != 5) {
                fprintf(stderr, "-- LOCAL_IP PEER_IP HTTP_PORT DNS_PORT\n");
                return 1;
        }
        uint32_t local_ip = inet_addr(argv[1]);
        http_peer = (struct sockaddr_in){.sin_family = AF_INET,
                                         .sin_port = htons(atoi(argv[3])),
                                         .sin_addr.s_addr = inet_addr(argv[2])};
        dns_peer = http_peer;
        dns_peer.sin_port = htons(atoi(argv[4]));
        struct rte_mempool *mp =
            rte_pktmbuf_pool_create("public_live", 16383, 0, 0,
                                    RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        assert(mp);
        net_context_set_mempool(mp);
        struct port_topology topology = port_init(0, mp, 1500);
        net_context_init(0, local_ip, topology.ipv4_mtu);
        unsigned owner = rte_lcore_id();
        assert(owner_timer_global_init() == 0);
        assert(socket_registry_init() == 0 && socket_owner_init(owner) == 0);
        assert(ring_init_owner(owner) == 0 && arp_table_init_owner(owner) == 0);
        assert(rx_dispatch_configure_workers(&owner, 1) == 0);
        struct stack_runtime_worker worker;
        assert(stack_runtime_worker_init(&worker, owner, 0,
                                         NSOCK_ID_DEFAULT_CAPACITY, mp,
                                         ring_instance(), NULL, NULL) == 0);
        worker.direct_rx_enabled = true;
        worker.direct_tx_enabled = true;
        worker.port_id = 0;
        worker.rx_queue_id = worker.tx_queue_id = 0;
        pthread_t app;
        assert(pthread_create(&app, NULL, application, NULL) == 0);
        assert(stack_runtime_worker_entry(&worker) == 0);
        pthread_join(app, NULL);
        assert(socket_owner_command_live() == 0);
        printf(
            "public live: TCP=%u/100 UDP=%u/100 ACCEPT=%u/20 SO_ERROR=PASS\n",
            tcp_ok, udp_ok, accept_ok);
        rte_eth_dev_stop(0);
        rte_eth_dev_close(0);
        return 0;
}
