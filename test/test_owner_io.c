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
        if (owner_io_socket_create_local(IPPROTO_UDP, &result->handle) != 0)
                return -1;
        result->status = owner_io_close(result->handle);
        return result->status;
}

int main(int argc, char **argv) {
        assert(rte_eal_init(argc, argv) >= 0);
        rte_timer_subsystem_init();
        assert(socket_registry_init() == 0);
        assert(socket_owner_init(rte_lcore_id()) == 0);

        struct nsock_handle handle;
        assert(owner_io_socket_create(IPPROTO_UDP, &handle) == 0);
        struct nsock *sk = socket_owner_resolve_local(handle);
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

        socket_owner_fini();
        socket_registry_fini();
        return 0;
}
