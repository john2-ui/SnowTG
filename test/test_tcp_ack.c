/* Exercise ACK coalescing through real ingress, application reads and egress. */
#include "../pro-stack/owner_io.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/socket_owner_internal.h"
#include "../pro-stack/net_context.h"
#include "../pro-stack/arp.h"
#include "../pro-stack/ring.h"
#include <assert.h>
#include <rte_eal.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <string.h>

/* Owner-only listener entry point; public applications use nlisten. */
extern int tcp_listen(struct nsock *sk, int backlog);

static struct rte_mempool *mp;
static struct rte_ring *out;
static uint32_t ack_backoff;

static void receive_flags(struct nsock *sk, uint32_t seq, uint8_t flags,
                          const char *data) {
        size_t len = strlen(data);
        struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
        assert(m != NULL);
        size_t size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)
                      + sizeof(struct rte_tcp_hdr) + len;
        struct rte_ether_hdr *eth = (void *)rte_pktmbuf_append(m, size);
        assert(eth != NULL);
        memset(eth, 0, size);
        eth->src_addr.addr_bytes[0] = 2;
        struct rte_ipv4_hdr *ip = (void *)(eth + 1);
        ip->version_ihl = 0x45;
        ip->total_length = rte_cpu_to_be_16(size - sizeof(*eth));
        ip->next_proto_id = IPPROTO_TCP;
        ip->src_addr = sk->u.tcp.remote_ip;
        ip->dst_addr = sk->local_ip;
        struct rte_tcp_hdr *tcp = (void *)(ip + 1);
        tcp->src_port = sk->u.tcp.remote_port;
        tcp->dst_port = sk->local_port;
        tcp->sent_seq = rte_cpu_to_be_32(seq);
        tcp->recv_ack = rte_cpu_to_be_32(sk->u.tcp.sent_seq +
            (((flags & RTE_TCP_SYN_FLAG) != 0) ||
             sk->u.tcp.status == TCP_STATUS_SYN_RECV) - ack_backoff);
        tcp->data_off = 5 << 4;
        tcp->tcp_flags = flags;
        tcp->rx_win = rte_cpu_to_be_16(65535);
        memcpy(tcp + 1, data, len);
        tcp->cksum = rte_ipv4_udptcp_cksum(ip, tcp);
        assert(tcp_ingress(m) == 0);
}

static void receive(struct nsock *sk, uint32_t seq, uint8_t flags,
                    const char *data) {
        receive_flags(sk, seq, flags | RTE_TCP_ACK_FLAG, data);
}

static void check_packet(uint32_t ack, size_t payload, bool fin) {
        struct rte_mbuf *m;
        assert(rte_ring_sc_dequeue(out, (void **)&m) == 0);
        struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(
            m, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
        struct rte_tcp_hdr *tcp = (void *)(ip + 1);
        assert(rte_be_to_cpu_32(tcp->recv_ack) == ack);
        assert(rte_be_to_cpu_16(ip->total_length) - sizeof(*ip) -
               (tcp->data_off >> 4) * 4 == payload);
        assert(!!(tcp->tcp_flags & RTE_TCP_FIN_FLAG) == fin);
        assert(tcp->tcp_flags & RTE_TCP_ACK_FLAG);
        assert(rte_be_to_cpu_16(tcp->rx_win) != 0);
        rte_pktmbuf_free(m);
}

static void cleanup(struct nsock *sk, struct nsock_handle handle) {
        bool closed = sk->app_closed;
        tcp_force_abort(sk, 0, "test-cleanup");
        if (!closed)
                assert(owner_io_close(handle) == 0);
        struct rte_mbuf *m;
        while (rte_ring_sc_dequeue(out, (void **)&m) == 0)
                rte_pktmbuf_free(m); /* Abort emits an RST. */
}

static struct nsock *new_stream(struct nsock_handle *handle, unsigned int n) {
        assert(owner_io_socket_create_local(IPPROTO_TCP, handle) == 0);
        struct nsock *sk = socket_owner_resolve_local(*handle);
        sk->local_ip = g_net.local_ip;
        sk->local_port = rte_cpu_to_be_16(10000 + n);
        sk->u.tcp.remote_ip = rte_cpu_to_be_32(0xc0000202);
        sk->u.tcp.remote_port = rte_cpu_to_be_16(80);
        sk->u.tcp.status = TCP_STATUS_ESTABLISHED;
        sk->u.tcp.recv_ack = 1000;
        sk->u.tcp.snd_wnd = 65535;
        assert(nsock_tcp_conn_register(sk) == 0);
        return sk;
}

int main(int argc, char **argv) {
        assert(rte_eal_init(argc, argv) >= 0);
        struct owner_timer_engine timers;
        assert(owner_timer_global_init() == 0);
        assert(owner_timer_engine_init(&timers, rte_lcore_id(), 1024) == 0);
        assert(socket_registry_init() == 0);
        assert(socket_owner_init(rte_lcore_id()) == 0);
        assert(ring_init_owner(rte_lcore_id()) == 0);
        assert(arp_table_init_owner(rte_lcore_id()) == 0);
        out = ring_instance()->out;
        mp = rte_pktmbuf_pool_create("ack_mp", 4095, 0, 0,
                                     RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        assert(mp != NULL);
        g_net.mp = mp;
        g_net.local_ip = rte_cpu_to_be_32(0xc0000201);
        unsigned int baseline = rte_mempool_avail_count(mp);
        for (unsigned int mode = 0; mode < 6; mode++) {
                struct nsock_handle handle;
                struct nsock *sk = new_stream(&handle, mode);
                if (mode == 0)
                        sk->u.tcp.rcvbuf_size = 6; /* Reopen a zero RX window. */
                /* Multiple reads of one response still require only one ACK. */
                receive(sk, 1000, 0, "abcdef");
                char buf[8];
                assert(owner_io_recv(handle, buf, 2) == 2);
                assert(owner_io_recv(handle, buf, 4) == 4);
                assert(sk->u.tcp.ack_pending);
                assert(sk->u.tcp.tx_queue_count == 0);
                if (mode != 0 && mode != 5)
                        assert(owner_io_send(handle, "request", 7) == 7);
                if (mode == 5)
                        assert(owner_io_close(handle) == 0); /* FIN carries ACK. */
                if (mode == 2)
                        sk->u.tcp.snd_wnd = 0;
                if (mode == 3)
                        sk->u.tcp.cc.cwnd = 0;
                if (mode == 4) {
                        /* Ring backpressure must not consume the pending ACK. */
                        while (rte_ring_sp_enqueue(out, sk) == 0) {}
                        assert(tcp_tx_flush(sk, mp) == SOCK_TX_FLUSH_RETRY);
                        assert(sk->u.tcp.ack_pending);
                        void *p;
                        while (rte_ring_sc_dequeue(out, &p) == 0)
                                assert(p == sk);
                }
                assert(tcp_tx_flush(sk, mp) == SOCK_TX_FLUSH_IDLE);
                assert(!sk->u.tcp.ack_pending);
                assert(rte_ring_count(out) == 1);
                check_packet(1006, mode == 1 || mode == 4 ? 7 : 0, mode == 5);
                cleanup(sk, handle);
        }
        /* Partial ACK releases bytes once; retransmitting retained data must
         * neither allocate a second payload nor grow unacked occupancy. */
        struct nsock_handle partial_handle;
        struct nsock *partial = new_stream(&partial_handle, 39);
        receive(partial, 1000, 0, "");
        assert(owner_io_send(partial_handle, "abcdefgh", 8) == 8);
        assert(tcp_tx_flush(partial, mp) == SOCK_TX_FLUSH_IDLE);
        check_packet(1000, 8, false);
        ack_backoff = 4;
        receive(partial, 1000, 0, "");
        ack_backoff = 0;
        struct owner_resource_snapshot before, after;
        assert(owner_io_resource_snapshot(&before) == 0);
        assert(before.values[OWNER_RESOURCE_tcp_sndbuf_bytes].current == 4);
        assert(before.values[OWNER_RESOURCE_tcp_unacked_bytes].current == 4);
        assert(owner_timer_arm_at(&partial->u.tcp.timer, owner_timer_now()) == 0);
        assert(owner_timer_poll(&timers) == 0);
        assert(tcp_tx_flush(partial, mp) == SOCK_TX_FLUSH_IDLE);
        check_packet(1000, 4, false);
        assert(owner_io_resource_snapshot(&after) == 0);
        assert(after.values[OWNER_RESOURCE_tcp_unacked_bytes].current == 4);
        assert(after.values[OWNER_RESOURCE_tcp_payload].current == before.values[OWNER_RESOURCE_tcp_payload].current);
        assert(after.values[OWNER_RESOURCE_tcp_unacked_bytes].peak == before.values[OWNER_RESOURCE_tcp_unacked_bytes].peak);
        receive(partial, 1000, 0, "");
        cleanup(partial, partial_handle);
        assert(owner_io_resource_snapshot(&after) == 0);
        assert(after.values[OWNER_RESOURCE_tcp_sndbuf_bytes].current == 0);
        assert(after.values[OWNER_RESOURCE_tcp_unacked_bytes].current == 0);

        /* Nagle holds a second short write until ACK; NODELAY reopening
         * releases it immediately. The first write never waits for an ACK. */
        struct nsock_handle nagle_handle;
        struct nsock *nagle = new_stream(&nagle_handle, 40);
        receive(nagle, 1000, 0, ""); /* learn the peer MAC */
        nagle->nodelay = false;
        assert(owner_io_send(nagle_handle, "a", 1) == 1);
        assert(tcp_tx_flush(nagle, mp) == SOCK_TX_FLUSH_IDLE);
        check_packet(1000, 1, false);
        assert(owner_io_send(nagle_handle, "b", 1) == 1);
        assert(tcp_tx_flush(nagle, mp) == SOCK_TX_FLUSH_IDLE);
        assert(rte_ring_empty(out));
        receive(nagle, 1000, 0, ""); /* ACK all bytes actually sent */
        assert(tcp_tx_flush(nagle, mp) == SOCK_TX_FLUSH_IDLE);
        check_packet(1000, 1, false);
        assert(owner_io_send(nagle_handle, "c", 1) == 1);
        assert(tcp_tx_flush(nagle, mp) == SOCK_TX_FLUSH_IDLE);
        assert(rte_ring_empty(out));
        nagle->nodelay = true;
        assert(tcp_tx_flush(nagle, mp) == SOCK_TX_FLUSH_IDLE);
        check_packet(1000, 1, false);
        /* A payload spanning multiple sndbuf chunks must make progress
         * through successive ACKs without duplicating or losing a suffix. */
        receive(nagle, 1000, 0, "");
        nagle->nodelay = false;
        char across_chunks[5000];
        memset(across_chunks, 'z', sizeof(across_chunks));
        assert(owner_io_send(nagle_handle, across_chunks, sizeof(across_chunks)) == sizeof(across_chunks));
        size_t total_payload = 0;
        for (unsigned round = 0; total_payload < sizeof(across_chunks) && round < 16; round++) {
                assert(tcp_tx_flush(nagle, mp) == SOCK_TX_FLUSH_IDLE);
                struct rte_mbuf *packet;
                while (rte_ring_sc_dequeue(out, (void **)&packet) == 0) {
                        struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(packet, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
                        struct rte_tcp_hdr *tcp = (void *)(ip + 1);
                        size_t len = rte_be_to_cpu_16(ip->total_length) - sizeof(*ip) - (tcp->data_off >> 4) * 4;
                        unsigned char *data = (void *)((char *)tcp + (tcp->data_off >> 4) * 4);
                        for (size_t j = 0; j < len; j++) assert(data[j] == 'z');
                        total_payload += len;
                        rte_pktmbuf_free(packet);
                }
                struct owner_resource_snapshot resources;
                assert(owner_io_resource_snapshot(&resources) == 0);
                assert(resources.values[OWNER_RESOURCE_tcp_sndbuf_bytes].current == nagle->u.tcp.sndbuf.len);
                assert(resources.values[OWNER_RESOURCE_tcp_unacked_bytes].current == nagle->u.tcp.sndbuf.unacked);
                receive(nagle, 1000, 0, "");
                assert(owner_io_resource_snapshot(&resources) == 0);
                assert(resources.values[OWNER_RESOURCE_tcp_unacked_bytes].current == 0);
                assert(resources.values[OWNER_RESOURCE_tcp_sndbuf_bytes].current == sizeof(across_chunks) - total_payload);
                assert(resources.values[OWNER_RESOURCE_tcp_sndbuf_bytes].peak == sizeof(across_chunks));
        }
        assert(total_payload == sizeof(across_chunks));
        cleanup(nagle, nagle_handle);

        /* REUSEADDR permits a bind beside TIME_WAIT, never reuse of the old
         * four-tuple. The owner and global flow indexes both retain that guard. */
        struct nsock_handle old_handle, replacement;
        struct nsock *old = new_stream(&old_handle, 41);
        old->reuseaddr = true;
        assert(nsock_bind_local(old, old->local_ip, old->local_port) == 0);
        tcp_stream_set_status(old, TCP_STATUS_TIME_WAIT);
        struct owner_resource_snapshot timewait;
        assert(owner_io_resource_snapshot(&timewait) == 0);
        assert(timewait.values[OWNER_RESOURCE_time_wait].current == 1);
        assert(timewait.values[OWNER_RESOURCE_time_wait].peak == 1);
        assert(owner_io_socket_create_local(IPPROTO_TCP, &replacement) == 0);
        struct nsock *fresh = socket_owner_resolve_local(replacement);
        fresh->reuseaddr = true;
        assert(nsock_bind_local(fresh, old->local_ip, old->local_port) == 0);
        fresh->u.tcp.remote_ip = old->u.tcp.remote_ip;
        fresh->u.tcp.remote_port = old->u.tcp.remote_port;
        assert(nsock_tcp_conn_register(fresh) == -EADDRINUSE);
        assert(owner_io_close(replacement) == 0);
        cleanup(old, old_handle);
        assert(owner_io_resource_snapshot(&timewait) == 0);
        assert(timewait.values[OWNER_RESOURCE_time_wait].current == 0);
        assert(timewait.values[OWNER_RESOURCE_time_wait].peak == 1);

        /* A wildcard listener's passive child owns the SYN destination,
         * otherwise the final ACK misses its four-tuple and accept stalls. */
        struct nsock_handle listening;
        assert(owner_io_socket_create_local(IPPROTO_TCP, &listening) == 0);
        struct nsock *listener = socket_owner_resolve_local(listening);
        assert(nsock_bind_local(listener, 0, rte_cpu_to_be_16(21988)) == 0);
        assert(tcp_listen(listener, 4) == 0);
        struct nsock tuple = {0};
        tuple.local_ip = g_net.local_ip;
        tuple.local_port = listener->local_port;
        tuple.u.tcp.remote_ip = rte_cpu_to_be_32(0xc0000202);
        tuple.u.tcp.remote_port = rte_cpu_to_be_16(30001);
        receive_flags(&tuple, 2000, RTE_TCP_SYN_FLAG, "");
        struct nsock *child = tcp_stream_search(tuple.u.tcp.remote_ip,
            tuple.local_ip, tuple.u.tcp.remote_port, tuple.local_port);
        assert(child != NULL && child->local_ip == g_net.local_ip);
        assert(child->u.tcp.status == TCP_STATUS_SYN_RECV);
        assert(tcp_tx_flush(child, mp) == SOCK_TX_FLUSH_IDLE);
        check_packet(2001, 0, false);
        receive(child, 2001, 0, "ping");
        assert(child->u.tcp.status == TCP_STATUS_ESTABLISHED);
        assert(rte_ring_count(listener->u.tcp.accept_queue) == 1);
        /* Closing the listener also reclaims its unaccepted child. */
        assert(owner_io_close(listening) == 0);
        struct rte_mbuf *leftover;
        while (rte_ring_sc_dequeue(out, (void **)&leftover) == 0)
                rte_pktmbuf_free(leftover);

        /* A final handshake ACK shares the first request, or leaves alone
         * this turn if the application has nothing to send. */
        for (unsigned int data = 0; data < 2; data++) {
                struct nsock_handle handshake;
                struct nsock *sk = new_stream(&handshake, 20 + data);
                sk->u.tcp.status = TCP_STATUS_SYN_SENT;
                receive(sk, 1000, RTE_TCP_SYN_FLAG, "");
                assert(sk->u.tcp.status == TCP_STATUS_ESTABLISHED);
                assert(sk->u.tcp.sent_seq == 1 && sk->u.tcp.snd_una == 1);
                assert(sk->u.tcp.ack_pending);
                if (data)
                        assert(owner_io_send(handshake, "request", 7) == 7);
                assert(tcp_tx_flush(sk, mp) == SOCK_TX_FLUSH_IDLE);
                assert(rte_ring_count(out) == 1);
                check_packet(1001, data ? 7 : 0, false);
                assert(!sk->u.tcp.ack_pending);
                cleanup(sk, handshake);
        }
        /* Duplicate/OOO ACKs and peer FIN remain on the immediate queue. */
        struct nsock_handle handle;
        struct nsock *sk = new_stream(&handle, 10);
        receive(sk, 1003, 0, "def");
        assert(sk->u.tcp.tx_queue_count == 1);
        assert(tcp_tx_flush(sk, mp) == SOCK_TX_FLUSH_IDLE);
        check_packet(1000, 0, false);
        receive(sk, 1000, 0, "abc");
        assert(tcp_tx_flush(sk, mp) == SOCK_TX_FLUSH_IDLE);
        check_packet(1006, 0, false);
        receive(sk, 1000, 0, "abc");
        assert(sk->u.tcp.tx_queue_count == 1);
        assert(tcp_tx_flush(sk, mp) == SOCK_TX_FLUSH_IDLE);
        check_packet(1006, 0, false);
        receive(sk, 1006, RTE_TCP_FIN_FLAG, "");
        assert(sk->u.tcp.tx_queue_count == 1);
        assert(tcp_tx_flush(sk, mp) == SOCK_TX_FLUSH_IDLE);
        check_packet(1007, 0, false);
        cleanup(sk, handle);
        assert(rte_ring_empty(out));
        assert(rte_mempool_avail_count(mp) == baseline);
        assert(timers.active == 0);
        struct owner_io_tcp_lifecycle_snapshot sockets = {0};
        assert(owner_io_tcp_lifecycle_snapshot(&sockets) == 0);
        assert(sockets.total == 0);
        owner_timer_engine_fini(&timers);
        socket_owner_fini();
        socket_registry_fini();
        arp_table_fini();
        ring_fini();
        rte_mempool_free(mp);
        puts("test_tcp_ack: PASS");
        return 0;
}
