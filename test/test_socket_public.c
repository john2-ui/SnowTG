/**
 * @file test_socket_public.c
 * @brief Cross-thread/owner regression of public sockets and managed commands.
 *
 * Paused-owner phases force queue saturation and cancellation before execution;
 * later phases verify generation filtering, cancellation without ingress and
 * shutdown. No NIC or peer is required. Final reference counts must reach zero.
 */
#include "../pro-stack/nepoll.h"
#include "../pro-stack/owner_io.h"
#include "../pro-stack/owner_timer.h"
#include "../pro-stack/rx_dispatch.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/socket_bind_internal.h"
#include "../pro-stack/socket_owner.h"
#include "../pro-stack/socket_owner_internal.h"
#include "../pro-stack/socket_public_internal.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_udp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static atomic_bool finished, pause_owner, owner_paused;
static atomic_uint returned;
static void *nonblocking_recv(void *arg) {
        char byte;
        assert(nrecvfrom(*(int *)arg, &byte, 1, MSG_DONTWAIT, NULL, NULL) ==
               -1);
        assert(errno == EAGAIN || errno == EBADF || errno == ECANCELED);
        atomic_fetch_add(&returned, 1);
        return NULL;
}
static void *close_socket(void *arg) {
        assert(nclose(*(int *)arg) == 0);
        return NULL;
}
static void *poll_waiter(void *arg) {
        struct nepoll_event event;
        assert(nepoll_wait(*(int *)arg, &event, 1, -1) == -1 && errno == EBADF);
        return NULL;
}
static void *blocked_recv(void *arg) {
        char data[32];
        nrecvfrom(*(int *)arg, data, sizeof(data), 0, NULL, NULL);
        return NULL;
}
static void *application(void *unused) {
        (void)unused;
        int fd = nsocket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        assert(fd >= 0);
        assert((nfcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
        int ep = nepoll_create(), ep2 = nepoll_create();
        assert(ep > 0 && ep2 > 0);
        struct nepoll_event event = {.events = NEPOLL_WRITE, .data = 42},
                            events[2];
        assert(nepoll_ctl(ep, NEPOLL_CTL_ADD, fd, &event) == 0);
        assert(nepoll_ctl(ep2, NEPOLL_CTL_ADD, fd, &event) == 0);
        assert(nepoll_wait(ep, events, 2, 0) == 1 && events[0].data == 42);
        assert(nepoll_wait(ep, events, 2, 0) == 1); /* level remains ready */
        assert(nepoll_wait(ep2, events, 2, 0) == 1);
        event.events = NEPOLL_READ;
        assert(nepoll_ctl(ep, NEPOLL_CTL_MOD, fd, &event) == 0);
        assert(nepoll_wait(ep, events, 2, 5) == 0);
        pthread_t poll_thread;
        assert(pthread_create(&poll_thread, NULL, poll_waiter, &ep) == 0);
        usleep(10000);
        assert(nepoll_close(ep) == 0);
        assert(pthread_join(poll_thread, NULL) == 0);
        ep = nepoll_create();
        char data[32];
        assert(nrecvfrom(fd, data, sizeof(data), 0, NULL, NULL) == -1 &&
               errno == EAGAIN);
        assert(nfcntl(fd, F_SETFL, 0) == 0);
        struct timeval tv = {.tv_usec = 20000};
        assert(nsetsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
        assert(nrecvfrom(fd, data, sizeof(data), 0, NULL, NULL) == -1 &&
               errno == EAGAIN);
        tv = (struct timeval){0};
        assert(nsetsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
        pthread_t blocked;
        assert(pthread_create(&blocked, NULL, blocked_recv, &fd) == 0);
        usleep(20000);
        assert(pthread_cancel(blocked) == 0);
        void *result;
        assert(pthread_join(blocked, &result) == 0 &&
               result == PTHREAD_CANCELED);
        assert(nclose(fd) == 0);
        assert(nclose(fd) == -1 && errno == EBADF);
        assert(nepoll_wait(ep2, events, 2, 0) == 0);
        assert(nepoll_close(ep) == 0 && nepoll_close(ep2) == 0);
        struct sockaddr_in addr = {.sin_family = AF_INET,
                                   .sin_port = htons(19876),
                                   .sin_addr.s_addr = htonl(0xc0000201)};
        int one = 1;
        int a = nsocket(AF_INET, SOCK_DGRAM, 0),
            b = nsocket(AF_INET, SOCK_DGRAM, 0);
        assert(a >= 0 && b >= 0);
        assert(nsetsockopt(a, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ==
               0);
        assert(nsetsockopt(b, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ==
               0);
        assert(nbind(a, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        struct nsock_handle first, second, restored;
        assert(socket_bind_udp_select(addr.sin_addr.s_addr, addr.sin_port,
                                      &first));
        assert(nbind(b, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(socket_bind_udp_select(addr.sin_addr.s_addr, addr.sin_port,
                                      &second));
        assert(first.id != second.id ||
               first.owner_lcore != second.owner_lcore);
        assert(nclose(b) == 0);
        assert(socket_bind_udp_select(addr.sin_addr.s_addr, addr.sin_port,
                                      &restored));
        assert(first.id == restored.id &&
               first.generation == restored.generation);
        assert(nclose(a) == 0);
        assert(!socket_bind_udp_select(addr.sin_addr.s_addr, addr.sin_port,
                                       &restored));
        /* Binding reservations may be shared, overlapping live listeners may
         * not. Closing the first listener must permit immediate replacement. */
        a = nsocket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        b = nsocket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        assert(nsetsockopt(a, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ==
               0);
        assert(nsetsockopt(b, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ==
               0);
        assert(nbind(a, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(nbind(b, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(nlisten(a, 8) == 0);
        assert(nlisten(b, 8) == -1 && errno == EADDRINUSE);
        assert(nclose(a) == 0);
        assert(nlisten(b, 8) == 0);
        assert(naccept4(b, NULL, NULL, SOCK_NONBLOCK) == -1 && errno == EAGAIN);
        assert(nfcntl(b, F_SETFL, 0) == 0);
        tv = (struct timeval){.tv_usec = 10000};
        assert(nsetsockopt(b, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
        assert(naccept(b, NULL, NULL) == -1 && errno == EAGAIN);
        assert(nclose(b) == 0);
        a = nsocket(AF_INET, SOCK_DGRAM, 0);
        b = nsocket(AF_INET, SOCK_DGRAM, 0);
        assert(nbind(a, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        addr.sin_addr.s_addr = INADDR_ANY;
        assert(nbind(b, (struct sockaddr *)&addr, sizeof(addr)) == -1 &&
               errno == EADDRINUSE);
        assert(nclose(a) == 0);
        assert(nbind(b, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(nsetsockopt(b, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ==
                   -1 &&
               errno == EINVAL);
        assert(nclose(b) == 0);
        fd = nsocket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        assert(fd >= 0);
        int value = 0;
        socklen_t size = sizeof(value);
        assert(ngetsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &size) == 0 &&
               value == 1);
        value = 0;
        assert(nsetsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value,
                           sizeof(value)) == 0);
        size = sizeof(value);
        assert(ngetsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &size) == 0 &&
               value == 0);
        assert(nclose(fd) == 0);
        /* With no packet ingress, the command deadline must cancel a TCP
         * handshake and leave the descriptor safe to inspect and close. */
        fd = nsocket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        tv = (struct timeval){.tv_usec = 10000};
        addr.sin_addr.s_addr = htonl(0xc0000202);
        assert(nsetsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0);
        assert(nconnect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1 &&
               errno == ETIMEDOUT);
        value = 0;
        size = sizeof(value);
        assert(ngetsockopt(fd, SOL_SOCKET, SO_ERROR, &value, &size) == 0 &&
               value == ETIMEDOUT);
        assert(nclose(fd) == 0);
        fd = nsocket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        assert(fd >= 0);
        assert(nconnect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1 &&
               errno == EINPROGRESS);
        assert(nconnect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1 &&
               errno == EALREADY);
        assert(nclose(fd) == 0);
        fd = nsocket(AF_INET, SOCK_DGRAM, 0);
        assert(fd >= 0);
        atomic_store(&pause_owner, true);
        while (!atomic_load(&owner_paused))
                sched_yield();
        pthread_t producers[32], closer, queued, backpressured;
        /* Cancel one admitted command before the paused owner dequeues it. */
        assert(pthread_create(&queued, NULL, blocked_recv, &fd) == 0);
        usleep(10000);
        assert(pthread_cancel(queued) == 0);
        assert(pthread_join(queued, &result) == 0 &&
               result == PTHREAD_CANCELED);
        for (unsigned i = 0; i < 32; i++)
                assert(pthread_create(&producers[i], NULL, nonblocking_recv,
                                      &fd) == 0);
        /* At least one immediate EAGAIN proves the data ring is saturated
         * while its consumer is deliberately stopped. */
        while (!atomic_load(&returned))
                sched_yield();
        /* Cancellation during producer backpressure must finish without
         * waiting for the deliberately stopped owner to free ring space. */
        assert(pthread_create(&backpressured, NULL, blocked_recv, &fd) == 0);
        usleep(10000);
        assert(pthread_cancel(backpressured) == 0);
        assert(pthread_join(backpressured, &result) == 0 &&
               result == PTHREAD_CANCELED);
        assert(pthread_create(&closer, NULL, close_socket, &fd) == 0);
        usleep(10000);
        atomic_store(&pause_owner, false);
        for (unsigned i = 0; i < 32; i++)
                assert(pthread_join(producers[i], NULL) == 0);
        assert(pthread_join(closer, NULL) == 0);
        assert(atomic_load(&returned) == 32);
        atomic_store(&finished, true);
        return NULL;
}
/** Exercise overflow using retired generations, which may occupy every ring
 * entry even though only one currently live socket needs a notification. */
static void ready_overflow(void) {
        struct nsock_handle handle;
        for (unsigned i = 0; i < 40; i++) {
                assert(owner_io_socket_create_local(IPPROTO_UDP, &handle) == 0);
                socket_owner_ready_post(socket_owner_resolve_local(handle),
                                        OWNER_IO_EV_READ);
                assert(owner_io_close(handle) == 0);
        }
        assert(owner_io_socket_create_local(IPPROTO_UDP, &handle) == 0);
        struct nsock *sk = socket_owner_resolve_local(handle);
        socket_owner_ready_post(sk, OWNER_IO_EV_READ | OWNER_IO_EV_WRITE);
        struct owner_io_event events[64];
        assert(owner_io_ready_burst(events, 64) == 1);
        assert(events[0].handle.id == handle.id &&
               events[0].handle.generation == handle.generation);
        assert(events[0].events == (OWNER_IO_EV_READ | OWNER_IO_EV_WRITE));
        assert(owner_io_ready_burst(events, 64) == 0);
        assert(owner_io_close(handle) == 0);
}

/** The selected UDP generation is immutable across a handoff, even when its
 * recipient closes and the same slot is immediately reused by another bind. */
static void udp_inflight_generation(void) {
        unsigned owner = rte_lcore_id();
        assert(rx_dispatch_configure_workers(&owner, 1) == 0);
        struct nsock_handle a, b, c;
        struct sockaddr_in addr = {.sin_family = AF_INET,
                                   .sin_port = htons(19910),
                                   .sin_addr.s_addr = htonl(0xc0000201)};
        assert(owner_io_socket_create_local(IPPROTO_UDP, &a) == 0);
        assert(owner_io_socket_create_local(IPPROTO_UDP, &b) == 0);
        socket_owner_resolve_local(a)->reuseaddr = true;
        socket_owner_resolve_local(b)->reuseaddr = true;
        assert(owner_io_bind(a, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(owner_io_bind(b, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        unsigned char bytes[sizeof(struct rte_ether_hdr) +
                            sizeof(struct rte_ipv4_hdr) +
                            sizeof(struct rte_udp_hdr)] = {0};
        struct rte_ether_hdr *eth = (void *)bytes;
        eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);
        struct rte_ipv4_hdr *ip = (void *)(eth + 1);
        ip->version_ihl = 0x45;
        ip->next_proto_id = IPPROTO_UDP;
        ip->dst_addr = addr.sin_addr.s_addr;
        struct rte_udp_hdr *udp = (void *)(ip + 1);
        udp->dst_port = addr.sin_port;
        struct rte_mbuf mbuf = {.buf_addr = bytes,
                                .data_len = sizeof(bytes),
                                .pkt_len = sizeof(bytes)};
        struct rx_dispatch_result result;
        rx_dispatch_classify(&mbuf, 0, &result);
        assert(mbuf.dynfield1[3] == b.id && mbuf.dynfield1[4] == b.generation);
        assert(owner_io_close(b) == 0);
        assert(owner_io_socket_create_local(IPPROTO_UDP, &c) == 0);
        socket_owner_resolve_local(c)->reuseaddr = true;
        assert(owner_io_bind(c, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(c.id == b.id && c.generation != b.generation);
        rx_dispatch_classify(&mbuf, 0, &result);
        assert(mbuf.dynfield1[4] == b.generation);
        assert(socket_owner_resolve_local(b) == NULL);
        assert(owner_io_close(c) == 0 && owner_io_close(a) == 0);
        rx_dispatch_reset();
}

static atomic_bool multi_done, remote_ready;
static int remote_owner(void *unused) {
        (void)unused;
        struct owner_timer_engine timer;
        assert(owner_timer_engine_init(&timer, rte_lcore_id(), 4096) == 0);
        atomic_store(&remote_ready, true);
        while (!atomic_load(&multi_done)) {
                socket_owner_process_commands();
                owner_timer_poll(&timer);
        }
        socket_owner_shutdown_local();
        owner_timer_engine_fini(&timer);
        return 0;
}
static void *multi_application(void *unused) {
        (void)unused;
        int a = nsocket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        int b = nsocket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        assert(a >= 0 && b >= 0);
        struct nsock_handle ah, bh, selected;
        uint32_t mask;
        assert(socket_public_snapshot(a, &ah, &mask) == 0);
        assert(socket_public_snapshot(b, &bh, &mask) == 0);
        assert(ah.owner_lcore != bh.owner_lcore);
        int ep = nepoll_create(), one = 1;
        struct nepoll_event event = {.events = NEPOLL_WRITE}, events[2];
        assert(nepoll_ctl(ep, NEPOLL_CTL_ADD, a, &event) == 0);
        assert(nepoll_ctl(ep, NEPOLL_CTL_ADD, b, &event) == 0);
        assert(nepoll_wait(ep, events, 2, 0) == 2);
        struct sockaddr_in addr = {.sin_family = AF_INET,
                                   .sin_port = htons(19877),
                                   .sin_addr.s_addr = htonl(0xc0000201)};
        assert(nsetsockopt(a, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ==
               0);
        assert(nsetsockopt(b, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ==
               0);
        assert(nbind(a, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(nbind(b, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(socket_bind_udp_select(addr.sin_addr.s_addr, addr.sin_port,
                                      &selected));
        assert(selected.owner_lcore == bh.owner_lcore && selected.id == bh.id);
        assert(nclose(b) == 0);
        assert(socket_bind_udp_select(addr.sin_addr.s_addr, addr.sin_port,
                                      &selected));
        assert(selected.owner_lcore == ah.owner_lcore && selected.id == ah.id);
        assert(nepoll_wait(ep, events, 2, 0) == 1 && events[0].fd == a);
        assert(nclose(a) == 0 && nepoll_close(ep) == 0);
        atomic_store(&multi_done, true);
        return NULL;
}

static atomic_bool shutdown_waiting;
static void *shutdown_application(void *unused) {
        (void)unused;
        /* A successful result deliberately left unpublished must be reaped
         * by its owner, rather than orphaning the newly allocated socket. */
        struct sock_cmd create = {.type = SOCK_CMD_CREATE};
        create.args.create.type = SOCK_DGRAM;
        create.args.create.protocol = IPPROTO_UDP;
        assert(socket_owner_call(&create) == 0);
        socket_owner_result_release(&create, false);
        int fd = nsocket(AF_INET, SOCK_DGRAM, 0);
        assert(fd >= 0);
        atomic_store(&shutdown_waiting, true);
        char byte;
        assert(nrecvfrom(fd, &byte, 1, 0, NULL, NULL) == -1 &&
               errno == ENETDOWN);
        assert(nclose(fd) == -1 && errno == ENETDOWN);
        return NULL;
}

/** Exercise cancellation without abandoning the caller, then reject a late
 * success notification while the caller's reference is still alive. */
static atomic_bool cancel_waiting, cancel_returned, cancel_observed;
static struct nsock_handle cancel_handle;
static struct sock_cmd *completed_create;
static void *cancel_application(void *unused) {
        (void)unused;
        struct sock_cmd create = {.type = SOCK_CMD_CREATE};
        create.args.create.type = SOCK_DGRAM;
        create.args.create.protocol = IPPROTO_UDP;
        assert(socket_owner_call(&create) == 0);
        cancel_handle = create.result_handle;
        completed_create = create.managed_result;
        atomic_store(&cancel_waiting, true);
        char byte;
        struct sock_cmd recv = {.type = SOCK_CMD_RECVFROM,
                                .handle = cancel_handle};
        recv.args.io.buf = &byte;
        recv.args.io.len = 1;
        assert(socket_owner_call(&recv) == -1 && errno == ECANCELED);
        atomic_store(&cancel_returned, true);
        while (!atomic_load(&cancel_observed))
                sched_yield();
        socket_owner_result_release(&create, false);
        return NULL;
}
static void cancellation_regression(void) {
        pthread_t caller;
        assert(pthread_create(&caller, NULL, cancel_application, NULL) == 0);
        struct sock_cmd *request = NULL;
        while (!request) {
                socket_owner_process_commands();
                if (atomic_load(&cancel_waiting)) {
                        struct nsock *sk =
                            socket_owner_resolve_local(cancel_handle);
                        assert(sk != NULL);
                        request = sk->recv_wait_head;
                }
        }
        socket_owner_cancel(request, ECANCELED);
        socket_owner_cancel(request, ETIMEDOUT); /* first reason wins */
        socket_owner_process_commands();
        /* CREATE remains held by the caller until cancel_observed. A late
         * error cannot overwrite its successful result or duplicate cleanup. */
        socket_owner_complete(completed_create, -1, ETIMEDOUT);
        assert(completed_create->result == 0 && completed_create->error == 0);
        while (!atomic_load(&cancel_returned))
                sched_yield();
        atomic_store(&cancel_observed, true);
        pthread_join(caller, NULL);
        socket_owner_process_commands();
}

int main(int argc, char **argv) {
        alarm(30);
        assert(rte_eal_init(argc, argv) >= 0);
        struct owner_timer_engine timer;
        assert(owner_timer_global_init() == 0);
        assert(owner_timer_engine_init(&timer, rte_lcore_id(), 4096) == 0);
        assert(socket_registry_init() == 0);
        assert(socket_owner_init_with_capacity(rte_lcore_id(), 16) == 0);
        pthread_t app;
        assert(pthread_create(&app, NULL, application, NULL) == 0);
        while (!atomic_load(&finished)) {
                if (atomic_load(&pause_owner)) {
                        atomic_store(&owner_paused, true);
                        while (atomic_load(&pause_owner))
                                sched_yield();
                        atomic_store(&owner_paused, false);
                }
                socket_owner_process_commands();
                owner_timer_poll(&timer);
        }
        pthread_join(app, NULL);
        socket_owner_process_commands();
        cancellation_regression();
        ready_overflow();
        udp_inflight_generation();
        unsigned remote = rte_get_next_lcore(rte_lcore_id(), 1, 0);
        assert(remote != RTE_MAX_LCORE);
        assert(socket_registry_init_owner(remote) == 0);
        assert(socket_owner_init_with_capacity(remote, 16) == 0);
        assert(rte_eal_remote_launch(remote_owner, NULL, remote) == 0);
        while (!atomic_load(&remote_ready))
                sched_yield();
        assert(pthread_create(&app, NULL, multi_application, NULL) == 0);
        while (!atomic_load(&multi_done)) {
                socket_owner_process_commands();
                owner_timer_poll(&timer);
        }
        pthread_join(app, NULL);
        assert(rte_eal_wait_lcore(remote) == 0);
        assert(pthread_create(&app, NULL, shutdown_application, NULL) == 0);
        while (!atomic_load(&shutdown_waiting))
                socket_owner_process_commands();
        /* Let the final receive be either queued or parked: both states must
         * complete during stop, without another ingress event. */
        usleep(10000);
        socket_owner_process_commands();
        socket_owner_shutdown_local();
        assert(pthread_join(app, NULL) == 0);
        assert(socket_owner_command_live() == 0);
        owner_timer_engine_fini(&timer);
        socket_owner_fini();
        socket_registry_fini();
        rte_eal_cleanup();
        puts("public socket lifecycle: PASS");
        return 0;
}
