#include "../pro-stack/owner_io.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/socket_owner_internal.h"

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <rte_byteorder.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_ring.h>
#include <rte_timer.h>

static void count_release(void *ctx) {
        unsigned int *count = ctx;

        (*count)++;
}

static unsigned int fake_flush_calls;
static int fake_flush_result;

static int fake_tx_flush(__attribute__((unused)) struct nsock *sk,
                         __attribute__((unused)) struct rte_mempool *mp) {
        fake_flush_calls++;
        return fake_flush_result;
}

static const struct sock_ops fake_tx_ops = {
    .name = "test-tx",
    .tx_flush = fake_tx_flush,
};

struct remote_owner_result {
        struct nsock_handle handle;
        int status;
};

static int remote_owner_entry(void *arg) {
        struct remote_owner_result *result = arg;
        unsigned int lcore_id = rte_lcore_id();

        result->status = -1;
        if (socket_registry_init_owner(lcore_id) != 0 ||
            socket_owner_init(lcore_id) != 0) {
                result->status = EAGAIN;
                return 0;
        }
        if (owner_io_socket_create(IPPROTO_UDP, &result->handle) != 0)
                return -1;
        result->status = owner_io_close(result->handle);
        return result->status;
}

int main(int argc, char **argv) {
        assert(rte_eal_init(argc, argv) >= 0);
        struct owner_timer_engine timer_engine;
        assert(owner_timer_global_init() == 0);
        assert(owner_timer_engine_init(&timer_engine, rte_lcore_id(),
                                       NSOCK_ID_DEFAULT_CAPACITY) == 0);
        assert(socket_registry_init() == 0);
        assert(socket_owner_init(rte_lcore_id()) == 0);

        struct nsock_handle handle;
        assert(owner_io_socket_create(IPPROTO_UDP, &handle) == 0);
        struct nsock *sk = socket_owner_resolve_local(handle);
        assert(sk != NULL);

        struct nsock_handle unbound_udp;
        assert(owner_io_socket_create(IPPROTO_UDP, &unbound_udp) == 0);
        uint32_t local_ip = rte_cpu_to_be_32(0xc0a81502);
        assert(owner_io_bind_ephemeral(unbound_udp, local_ip) == 0);
        sk = socket_owner_resolve_local(unbound_udp);
        assert(sk != NULL);
        assert(sk->local_ip == local_ip);
        assert(sk->local_port != 0);
        uint16_t local_port = sk->local_port;
        assert(nsock_from_ip_port(local_ip, local_port, IPPROTO_UDP) == sk);
        assert(owner_io_bind_ephemeral(unbound_udp, local_ip) == 0);
        assert(socket_owner_resolve_local(unbound_udp)->local_port ==
               local_port);
        assert(owner_io_close(unbound_udp) == 0);
        assert(nsock_from_ip_port(local_ip, local_port, IPPROTO_UDP) == NULL);
        sk = socket_owner_resolve_local(handle);
        assert(sk != NULL);

        unsigned int udp_release_count = 0;
        struct nsock_handle local_udp;
        assert(owner_io_socket_create_local(IPPROTO_UDP, &local_udp) == 0);
        sk = socket_owner_resolve_local(local_udp);
        assert(sk != NULL);
        assert(sk->io_mode == NSOCK_IO_OWNER_LOCAL);
        assert(sk->recv_buf == NULL);
        assert(sk->send_buf == NULL);
        assert(owner_io_set_release_observer(local_udp, count_release,
                                             &udp_release_count) == 0);
        assert(owner_io_close(local_udp) == 0);
        assert(udp_release_count == 1);
        sk = socket_owner_resolve_local(handle);
        assert(sk != NULL);

        char buf[8];
        errno = 0;
        assert(owner_io_recvfrom(handle, buf, sizeof(buf), NULL, NULL) == -1);
        assert(errno == EAGAIN);
        assert(sk->recv_wait_head == NULL);

        struct sockaddr addr_buf;
        socklen_t short_addr_len = sizeof(sa_family_t);
        errno = 0;
        assert(owner_io_recvfrom(handle, buf, sizeof(buf), &addr_buf,
                                 &short_addr_len) == -1);
        assert(errno == EINVAL);

        struct sockaddr_in invalid_peer = {
            .sin_family = AF_UNSPEC,
        };
        errno = 0;
        assert(owner_io_sendto(handle, buf, sizeof(buf),
                               (struct sockaddr *)&invalid_peer,
                               sizeof(invalid_peer)) == -1);
        assert(errno == EINVAL);
        errno = 0;
        assert(owner_io_sendto(handle, buf, sizeof(buf),
                               (struct sockaddr *)&invalid_peer,
                               sizeof(sa_family_t)) == -1);
        assert(errno == EINVAL);

        socket_owner_ready_post(sk, OWNER_IO_EV_READ);
        socket_owner_ready_post(sk, OWNER_IO_EV_WRITE);
        struct owner_io_event event[2];
        assert(owner_io_ready_burst(event, 2) == 1);
        assert(event[0].handle.id == handle.id);
        assert(event[0].handle.generation == handle.generation);
        assert(event[0].events == (OWNER_IO_EV_READ | OWNER_IO_EV_WRITE));

        socket_owner_ready_post(sk, OWNER_IO_EV_READ);
        assert(owner_io_close(handle) == 0);
        assert(owner_io_ready_burst(event, 2) == 0);

        struct nsock_handle wrong_owner = handle;
        wrong_owner.owner_lcore++;
        errno = 0;
        assert(owner_io_recvfrom(wrong_owner, buf, sizeof(buf), NULL, NULL) ==
               -1);
        assert(errno == EPERM);

        struct nsock_handle tcp_handle;
        assert(owner_io_socket_create(IPPROTO_TCP, &tcp_handle) == 0);
        struct sockaddr_in peer = {
            .sin_family = AF_INET,
            .sin_port = rte_cpu_to_be_16(80),
            .sin_addr.s_addr = rte_cpu_to_be_32(0x7f000001),
        };
        errno = 0;
        assert(owner_io_connect(tcp_handle, (struct sockaddr *)&peer,
                                sizeof(sa_family_t)) == -1);
        assert(errno == EINVAL);
        errno = 0;
        assert(owner_io_connect(tcp_handle, (struct sockaddr *)&peer,
                                sizeof(peer)) == -1);
        assert(errno == EINPROGRESS);
        assert(owner_io_close(tcp_handle) == 0);

        unsigned int release_count = 0;
        struct nsock_handle local_tcp;
        assert(owner_io_socket_create_local(IPPROTO_TCP, &local_tcp) == 0);
        sk = socket_owner_resolve_local(local_tcp);
        assert(sk != NULL);
        assert(sk->io_mode == NSOCK_IO_OWNER_LOCAL);
        assert(sk->recv_buf == NULL);
        assert(sk->send_buf == NULL);
        const struct sock_ops *local_ops = sk->ops;
        struct nsock_tx_metrics tx_metrics = {0};

        /*
         * The dirty queue is owner-local and coalesced independently of the
         * protocol queues.  A fake ops vector keeps this test independent of
         * NIC/ARP state while exercising drain, retry, and ARP wakeup.
         */
        sk->ops = &fake_tx_ops;
        fake_flush_calls = 0;
        fake_flush_result = SOCK_TX_FLUSH_IDLE;
        nsock_tx_metrics_take(&tx_metrics);
        nsock_tx_mark_dirty(sk);
        nsock_tx_mark_dirty(sk);
        nsock_tx_metrics_take(&tx_metrics);
        assert(tx_metrics.dirty_enqueues == 1);
        assert(tx_metrics.dirty_dedup_hits == 1);
        assert(tx_metrics.dirty_depth == 1);
        assert(nsock_tx_dirty_drain(NULL, 1) == 1);
        nsock_tx_metrics_take(&tx_metrics);
        assert(fake_flush_calls == 1);
        assert(tx_metrics.dirty_dequeues == 1);
        assert(tx_metrics.flush_calls == 1);
        assert(tx_metrics.dirty_depth == 0);

        fake_flush_result = SOCK_TX_FLUSH_RETRY;
        nsock_tx_mark_dirty(sk);
        assert(nsock_tx_dirty_drain(NULL, 1) == 1);
        nsock_tx_metrics_take(&tx_metrics);
        assert(tx_metrics.dirty_requeues == 1);
        assert(tx_metrics.dirty_depth == 1);
        fake_flush_result = SOCK_TX_FLUSH_IDLE;
        assert(nsock_tx_dirty_drain(NULL, 1) == 1);
        nsock_tx_metrics_take(&tx_metrics);
        assert(tx_metrics.dirty_depth == 0);

        nsock_tx_mark_dirty(sk);
        nsock_tx_arp_wait(sk, 0x01020304U);
        nsock_tx_metrics_take(&tx_metrics);
        assert(tx_metrics.arp_waits == 1);
        assert(tx_metrics.dirty_depth == 0);
        nsock_tx_arp_resolved(0x01020304U);
        nsock_tx_metrics_take(&tx_metrics);
        assert(tx_metrics.arp_wakeups == 1);
        assert(tx_metrics.dirty_depth == 1);
        assert(nsock_tx_dirty_drain(NULL, 1) == 1);
        nsock_tx_metrics_take(&tx_metrics);
        assert(tx_metrics.dirty_depth == 0);
        nsock_tx_mark_dirty(sk);
        nsock_tx_arp_wait(sk, 0x01020304U);
        sk->ops = local_ops;

        assert(owner_io_set_release_observer(local_tcp, count_release,
                                             &release_count) == 0);
        assert(owner_io_close(local_tcp) == 0);
        assert(release_count == 1);
        /* nsock_free() must have removed the ARP-wait pointer. */
        nsock_tx_arp_resolved(0x01020304U);

        /* Close policy: zero linger aborts immediately, while graceful and
         * positive asynchronous linger remain visible to lifecycle census
         * until owner-local forced cleanup. */
        unsigned int lifecycle_release_count = 0;
        struct nsock_handle zero_linger;
        assert(owner_io_socket_create_local(IPPROTO_TCP, &zero_linger) == 0);
        sk = socket_owner_resolve_local(zero_linger);
        assert(sk != NULL);
        sk->u.tcp.status = TCP_STATUS_ESTABLISHED;
        sk->u.tcp.linger_enabled = true;
        sk->u.tcp.linger_seconds = 0;
        assert(owner_io_set_release_observer(zero_linger, count_release,
                                             &lifecycle_release_count) == 0);
        assert(owner_io_close(zero_linger) == 0);
        assert(lifecycle_release_count == 1);

        struct nsock_handle graceful;
        struct nsock_handle positive_linger;
        struct nsock_handle positive_deferred;
        assert(owner_io_socket_create_local(IPPROTO_TCP, &graceful) == 0);
        sk = socket_owner_resolve_local(graceful);
        assert(sk != NULL);
        sk->u.tcp.status = TCP_STATUS_ESTABLISHED;
        assert(owner_io_set_release_observer(graceful, count_release,
                                             &lifecycle_release_count) == 0);
        assert(owner_io_close(graceful) == 0);

        assert(owner_io_socket_create_local(IPPROTO_TCP, &positive_linger) ==
               0);
        sk = socket_owner_resolve_local(positive_linger);
        assert(sk != NULL);
        sk->u.tcp.status = TCP_STATUS_ESTABLISHED;
        sk->u.tcp.linger_enabled = true;
        sk->u.tcp.linger_seconds = 5;
        assert(owner_io_set_release_observer(positive_linger, count_release,
                                             &lifecycle_release_count) == 0);
        assert(owner_io_close(positive_linger) == 0);
        assert(owner_timer_is_armed(&sk->u.tcp.timer));
        assert(sk->u.tcp.close_deadline_cycles != 0);

        assert(owner_io_socket_create_local(IPPROTO_TCP, &positive_deferred) ==
               0);
        sk = socket_owner_resolve_local(positive_deferred);
        assert(sk != NULL);
        sk->u.tcp.status = TCP_STATUS_ESTABLISHED;
        sk->u.tcp.linger_enabled = true;
        sk->u.tcp.linger_seconds = 5;
        assert(sk->ops->send(sk, "x", 1, MSG_DONTWAIT) == 1);
        assert(owner_io_set_release_observer(positive_deferred, count_release,
                                             &lifecycle_release_count) == 0);
        assert(owner_io_close(positive_deferred) == 0);
        sk = socket_owner_resolve_local(positive_deferred);
        assert(sk != NULL);
        assert(sk->u.tcp.fin_deferred);
        assert(owner_timer_is_armed(&sk->u.tcp.timer));

        struct owner_io_tcp_lifecycle_snapshot lifecycle = {0};
        assert(owner_io_tcp_lifecycle_snapshot(&lifecycle) == 0);
        assert(lifecycle.total == 3);
        assert(lifecycle.app_closed == 3);
        assert(lifecycle.states[TCP_STATUS_FIN_WAIT_1] == 3);
        assert(owner_io_tcp_force_cleanup() == 3);
        assert(lifecycle_release_count == 4);
        assert(timer_engine.active == 0);

        /* Listener cleanup owns both accept-queued and half-open children;
         * force cleanup must retire them without a registry-wide scan or a
         * dangling accept_queue entry. */
        struct nsock_handle listener_handle;
        struct sockaddr_in listener_addr = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = local_ip,
            .sin_port = rte_cpu_to_be_16(49100),
        };
        unsigned int listener_release_count = 0;
        assert(owner_io_socket_create_local(IPPROTO_TCP, &listener_handle) ==
               0);
        assert(owner_io_bind(listener_handle,
                             (const struct sockaddr *)&listener_addr,
                             sizeof(listener_addr)) == 0);
        struct nsock *listener =
            socket_owner_resolve_local(listener_handle);
        assert(listener != NULL);
        assert(listener->ops->listen(listener, 2) == 0);
        nsock_set_release_observer(listener, count_release,
                                   &listener_release_count);

        struct nsock *half_open = nsock_alloc(IPPROTO_TCP);
        struct nsock *accept_queued = nsock_alloc(IPPROTO_TCP);
        assert(half_open != NULL && accept_queued != NULL);
        assert(socket_owner_adopt(half_open) == 0);
        assert(socket_owner_adopt(accept_queued) == 0);
        half_open->u.tcp.status = TCP_STATUS_SYN_RECV;
        accept_queued->u.tcp.status = TCP_STATUS_ESTABLISHED;
        tcp_listener_child_attach(listener, half_open);
        tcp_listener_child_attach(listener, accept_queued);
        listener->u.tcp.syn_pending = 1;
        assert(rte_ring_mp_enqueue(listener->u.tcp.accept_queue,
                                   accept_queued) == 0);
        nsock_set_release_observer(half_open, count_release,
                                   &listener_release_count);
        nsock_set_release_observer(accept_queued, count_release,
                                   &listener_release_count);

        memset(&lifecycle, 0, sizeof(lifecycle));
        assert(owner_io_tcp_lifecycle_snapshot(&lifecycle) == 0);
        assert(lifecycle.total == 3);
        assert(lifecycle.states[TCP_STATUS_LISTEN] == 1);
        assert(lifecycle.states[TCP_STATUS_SYN_RECV] == 1);
        assert(lifecycle.states[TCP_STATUS_ESTABLISHED] == 1);
        assert(owner_io_tcp_force_cleanup() == 3);
        assert(listener_release_count == 3);
        assert(timer_engine.active == 0);
        assert(socket_owner_resolve_local(listener_handle) == NULL);

        unsigned int worker_lcore = rte_get_next_lcore(rte_lcore_id(), 1, 0);
        if (worker_lcore != RTE_MAX_LCORE && rte_eal_has_hugepages()) {
                struct remote_owner_result remote = {0};

                assert(rte_eal_remote_launch(remote_owner_entry, &remote,
                                             worker_lcore) == 0);
                assert(rte_eal_wait_lcore(worker_lcore) == 0);
                if (remote.status == 0) {
                        assert(remote.handle.owner_lcore == worker_lcore);
                        errno = 0;
                        assert(owner_io_recvfrom(remote.handle, buf,
                                                 sizeof(buf), NULL,
                                                 NULL) == -1);
                        assert(errno == EPERM);
                } else {
                        assert(remote.status == EAGAIN);
                }
        }

        /*
         * Capacity-aware owner initialization must reject the third live
         * socket, then reuse a retired slot with a new generation.
         */
        owner_timer_engine_fini(&timer_engine);
        socket_owner_fini();
        socket_registry_fini();
        assert(socket_registry_init_owner_with_capacity(rte_lcore_id(), 2) ==
               0);
        assert(socket_owner_init_with_capacity(rte_lcore_id(), 2) == 0);
        struct nsock_handle capacity_handles[2];
        struct nsock_handle replacement;
        assert(owner_io_socket_create_local(IPPROTO_UDP,
                                            &capacity_handles[0]) == 0);
        assert(owner_io_socket_create_local(IPPROTO_UDP,
                                            &capacity_handles[1]) == 0);
        errno = 0;
        assert(owner_io_socket_create_local(IPPROTO_UDP, &replacement) == -1);
        assert(errno == ENFILE);
        assert(owner_io_close(capacity_handles[0]) == 0);
        assert(owner_io_socket_create_local(IPPROTO_UDP, &replacement) == 0);
        assert(replacement.id == capacity_handles[0].id);
        assert(replacement.generation != capacity_handles[0].generation);
        errno = 0;
        assert(socket_owner_resolve_local(capacity_handles[0]) == NULL);
        assert(errno == EBADF);
        assert(owner_io_close(capacity_handles[1]) == 0);
        assert(owner_io_close(replacement) == 0);

        socket_owner_fini();
        socket_registry_fini();
        return 0;
}
