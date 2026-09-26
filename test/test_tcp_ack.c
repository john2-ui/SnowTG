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

static struct rte_mempool *mp;
static struct rte_ring *out;

static void receive(struct nsock *sk, uint32_t seq, uint8_t flags,
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
            ((flags & RTE_TCP_SYN_FLAG) != 0));
        tcp->data_off = 5 << 4;
        tcp->tcp_flags = flags | RTE_TCP_ACK_FLAG;
        tcp->rx_win = rte_cpu_to_be_16(65535);
        memcpy(tcp + 1, data, len);
        tcp->cksum = rte_ipv4_udptcp_cksum(ip, tcp);
        assert(tcp_ingress(m) == 0);
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
