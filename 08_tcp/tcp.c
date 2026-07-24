/**
 * @file tcp.c
 * @brief TCP control block, table-driven state machine, encode/egress, and the
 *        tcp_ops vector consumed by the unified socket layer.
 *
 * Inbound:  tcp_ingress -> state handler -> recv_buf (ESTABLISHED) or send_buf
 * Outbound: tcp_tx_flush -> arp resolve -> out ring -> NIC
 */
#include "tcp.h"

#include "arp.h"
#include "config.h"
#include "list.h"
#include "log.h"
#include "net_context.h"
#include "pkt_frame.h"
#include "ring.h"
#include "socket.h"

#include <netinet/in.h>
#include <netinet/ip.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf_core.h>
#include <rte_tcp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *tcp_status_str(TCP_STATUS s) {
        switch (s) {
        case TCP_STATUS_CLOSED:
                return "CLOSED";
        case TCP_STATUS_LISTEN:
                return "LISTEN";
        case TCP_STATUS_SYN_SENT:
                return "SYN_SENT";
        case TCP_STATUS_SYN_RECV:
                return "SYN_RECV";
        case TCP_STATUS_ESTABLISHED:
                return "ESTABLISHED";
        case TCP_STATUS_CLOSE_WAIT:
                return "CLOSE_WAIT";
        case TCP_STATUS_LAST_ACK:
                return "LAST_ACK";
        case TCP_STATUS_TIME_WAIT:
                return "TIME_WAIT";
        case TCP_STATUS_CLOSING:
                return "CLOSING";
        case TCP_STATUS_FIN_WAIT_1:
                return "FIN_WAIT_1";
        case TCP_STATUS_FIN_WAIT_2:
                return "FIN_WAIT_2";
        default:
                return "UNKNOWN";
        }
}

static const char *tcp_flags_str(uint8_t flags) {
        static char buf[64];
        int n = 0;
        buf[0] = '\0';
        if (flags & RTE_TCP_SYN_FLAG)
                n += snprintf(buf + n, sizeof(buf) - (size_t)n, "SYN ");
        if (flags & RTE_TCP_ACK_FLAG)
                n += snprintf(buf + n, sizeof(buf) - (size_t)n, "ACK ");
        if (flags & RTE_TCP_FIN_FLAG)
                n += snprintf(buf + n, sizeof(buf) - (size_t)n, "FIN ");
        if (flags & RTE_TCP_RST_FLAG)
                n += snprintf(buf + n, sizeof(buf) - (size_t)n, "RST ");
        if (flags & RTE_TCP_PSH_FLAG)
                n += snprintf(buf + n, sizeof(buf) - (size_t)n, "PSH ");
        if (n == 0)
                snprintf(buf, sizeof(buf), "0x%02x", flags);
        else if (n > 0 && buf[n - 1] == ' ')
                buf[n - 1] = '\0';
        return buf;
}

struct nsock *tcp_stream_search(uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port) {
        return nsock_from_4tuple(remote_ip, local_ip, remote_port, local_port,
                                 IPPROTO_TCP);
}

/*
 * Process-wide ISN generator: seeded once from time(NULL) on first use, then
 * advanced by a fixed step per connection. This avoids re-seeding with
 * time(NULL) on every stream (which made ISNs predictable and identical
 * within the same second).
 */
static uint32_t tcp_isn_state;
static int tcp_isn_inited;
static uint32_t tcp_next_isn(void) {
        if (!tcp_isn_inited) {
                tcp_isn_state = (uint32_t)time(NULL);
                tcp_isn_inited = 1;
        }
        tcp_isn_state += 0x9e3779b1u; /* golden-ratio-ish step per stream */
        return tcp_isn_state;
}

struct nsock *tcp_stream_create(uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port) {
        int fd = fd_alloc();
        if (fd < 0)
                rte_exit(EXIT_FAILURE, "tcp: fd table full\n");
        struct nsock *sk = nsock_alloc(fd, IPPROTO_TCP);
        if (sk == NULL)
                rte_exit(EXIT_FAILURE, "nsock_alloc(tcp) failed\n");

        sk->local_ip = local_ip;
        sk->local_port = local_port;
        sk->u.tcp.remote_ip = remote_ip;
        sk->u.tcp.remote_port = remote_port;
        sk->u.tcp.status = TCP_STATUS_LISTEN;

        sk->u.tcp.sent_seq = tcp_next_isn();
        sk->u.tcp.recv_ack = 0;

        LOG_INFO("tcp stream create " IP_FMT ":%u -> " IP_FMT
                 ":%u isn=%u status=%s fd=%d",
                 IP_ARG(remote_ip), rte_be_to_cpu_16(remote_port),
                 IP_ARG(local_ip), rte_be_to_cpu_16(local_port),
                 sk->u.tcp.sent_seq, tcp_status_str(sk->u.tcp.status), fd);
        return sk;
}

void tcp_stream_set_status(struct nsock *sk, TCP_STATUS new_status) {
        TCP_STATUS old = sk->u.tcp.status;
        sk->u.tcp.status = new_status;
        LOG_INFO("tcp status %s -> %s " IP_FMT ":%u <-> " IP_FMT ":%u",
                 tcp_status_str(old), tcp_status_str(new_status),
                 IP_ARG(sk->u.tcp.remote_ip),
                 rte_be_to_cpu_16(sk->u.tcp.remote_port), IP_ARG(sk->local_ip),
                 rte_be_to_cpu_16(sk->local_port));
}

static struct rte_ipv4_hdr *tcp_ipv4_header(struct rte_mbuf *mbuf) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        return (struct rte_ipv4_hdr *)(eth + 1);
}

static struct tcp_fragment *tcp_fragment_alloc(void) {
        struct tcp_fragment *f =
            rte_malloc("tcp_fragment", sizeof(struct tcp_fragment), 0);
        if (f == NULL)
                rte_exit(EXIT_FAILURE, "rte_malloc(tcp_fragment) failed\n");
        memset(f, 0, sizeof(*f));
        return f;
}

static int tcp_state_listen(struct nsock *sk, struct rte_tcp_hdr *hdr,
                            struct rte_mbuf *mbuf) {
        (void)mbuf;
        if (!(hdr->tcp_flags & RTE_TCP_SYN_FLAG)) {
                LOG_DEBUG("tcp LISTEN ignore flags=%s from " IP_FMT ":%u",
                          tcp_flags_str(hdr->tcp_flags),
                          IP_ARG(sk->u.tcp.remote_ip),
                          rte_be_to_cpu_16(sk->u.tcp.remote_port));
                return 0;
        }
        if (sk->u.tcp.status != TCP_STATUS_LISTEN)
                return 0;

        struct tcp_fragment *f = tcp_fragment_alloc();
        f->src_port = sk->local_port;
        f->dst_port = sk->u.tcp.remote_port;
        f->sent_seq = sk->u.tcp.sent_seq;
        f->recv_ack = ntohl(hdr->sent_seq) + 1;
        f->tcp_flags = RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG;
        f->data_off = (5 << 4);
        f->rx_win = TCP_INITIAL_WINDOW_SIZE;
        sk->u.tcp.recv_ack = f->recv_ack;

        LOG_INFO("tcp handshake [1/3] SYN rx " IP_FMT ":%u -> " IP_FMT
                 ":%u seq=%u; reply SYN+ACK seq=%u ack=%u",
                 IP_ARG(sk->u.tcp.remote_ip),
                 rte_be_to_cpu_16(sk->u.tcp.remote_port), IP_ARG(sk->local_ip),
                 rte_be_to_cpu_16(sk->local_port), ntohl(hdr->sent_seq),
                 f->sent_seq, f->recv_ack);

        rte_ring_mp_enqueue(sk->send_buf, f);
        tcp_stream_set_status(sk, TCP_STATUS_SYN_RECV);
        return 0;
}

static int tcp_state_syn_recv(struct nsock *sk, struct rte_tcp_hdr *hdr,
                              struct rte_mbuf *mbuf) {
        (void)mbuf;
        if (!(hdr->tcp_flags & RTE_TCP_ACK_FLAG)) {
                LOG_DEBUG("tcp SYN_RECV ignore flags=%s from " IP_FMT ":%u",
                          tcp_flags_str(hdr->tcp_flags),
                          IP_ARG(sk->u.tcp.remote_ip),
                          rte_be_to_cpu_16(sk->u.tcp.remote_port));
                return 0;
        }
        if (sk->u.tcp.status != TCP_STATUS_SYN_RECV)
                return 0;

        uint32_t acknum = ntohl(hdr->recv_ack);
        uint32_t expect = sk->u.tcp.sent_seq + 1;
        if (acknum != expect) {
                LOG_WARN(
                    "tcp handshake ACK mismatch " IP_FMT ":%u ack=%u expect=%u",
                    IP_ARG(sk->u.tcp.remote_ip),
                    rte_be_to_cpu_16(sk->u.tcp.remote_port), acknum, expect);
                return 0;
        }

        LOG_INFO("tcp handshake [3/3] ACK rx " IP_FMT ":%u -> " IP_FMT
                 ":%u ack=%u",
                 IP_ARG(sk->u.tcp.remote_ip),
                 rte_be_to_cpu_16(sk->u.tcp.remote_port), IP_ARG(sk->local_ip),
                 rte_be_to_cpu_16(sk->local_port), acknum);

        sk->u.tcp.sent_seq += 1; /* the SYN consumes one sequence number */
        tcp_stream_set_status(sk, TCP_STATUS_ESTABLISHED);
        return 0;
}

static int tcp_state_established(struct nsock *sk, struct rte_tcp_hdr *hdr,
                                 struct rte_mbuf *mbuf) {
        uint8_t hdrlen = (hdr->data_off >> 4) * 4;
        uint16_t payload_len =
            rte_be_to_cpu_16(tcp_ipv4_header(mbuf)->total_length) -
            sizeof(struct rte_ipv4_hdr) - hdrlen;
        if (payload_len == 0)
                return 0; /* pure ACK: caller frees mbuf */

        /* TODO: validate that hdr->sent_seq matches sk->u.tcp.recv_ack
         * (in-order delivery). Out-of-order, duplicate, or overlapping segments
         * are currently neither detected nor reassembled -- they are delivered
         * as they arrive, which corrupts the byte stream. */
        sk->u.tcp.recv_ack = ntohl(hdr->sent_seq) + payload_len;
        /* TODO: enqueue a pure ACK fragment onto sk->send_buf so the peer
         * learns sk->u.tcp.recv_ack. Today no ACK is ever sent for received
         * data, so the peer cannot retire its in-flight bytes. */
        /* TODO: honor the peer's advertised rx_win (hdr->rx_win) before letting
         * tcp_send enqueue more data -- there is no flow control / sliding
         * window yet. */

        if (rte_ring_sp_enqueue(sk->recv_buf, mbuf) != 0) {
                LOG_ERROR("tcp recv_buf full for fd=%d, dropping", sk->fd);
                return 0; /* caller frees mbuf */
        }
        pthread_mutex_lock(&sk->mutex);
        pthread_cond_signal(&sk->cond);
        pthread_mutex_unlock(&sk->mutex);
        return 1; /* mbuf ownership transferred to recv_buf */
}

/*
 * Default handler for every state that is not yet implemented. Each such state
 * needs its own real handler instead of this drop stub:
 *
 *   TCP_STATUS_CLOSED      -> TODO: active open (send SYN) and RST handling.
 *   TCP_STATUS_SYN_SENT    -> TODO: client-side handshake: react to inbound
 *                             SYN+ACK by sending ACK and entering ESTABLISHED.
 *   TCP_STATUS_FIN_WAIT_1  -> TODO: teardown -- our FIN sent, await ACK.
 *   TCP_STATUS_FIN_WAIT_2  -> TODO: teardown -- await peer FIN, then ACK +
 * TIME_WAIT. TCP_STATUS_CLOSING     -> TODO: simultaneous close path.
 *   TCP_STATUS_CLOSE_WAIT  -> TODO: peer FIN received; app must close, then
 * send FIN. TCP_STATUS_LAST_ACK    -> TODO: our FIN sent on a CLOSE_WAIT; await
 * final ACK. TCP_STATUS_TIME_WAIT   -> TODO: 2MSL wait before freeing the
 * socket.
 *
 * Until these land, tcp_close() simply drops the queues without a FIN exchange.
 */
static int tcp_state_drop(struct nsock *sk, struct rte_tcp_hdr *hdr,
                          struct rte_mbuf *mbuf) {
        (void)sk;
        (void)hdr;
        (void)mbuf;
        return 0; /* TODO: implement the per-state handlers listed above. */
}

struct tcp_state_ops {
        int (*handle)(struct nsock *sk, struct rte_tcp_hdr *hdr,
                      struct rte_mbuf *mbuf);
};

static const struct tcp_state_ops tcp_state_ops[TCP_STATUS_MAX] = {
    [TCP_STATUS_CLOSED] = {tcp_state_drop},
    [TCP_STATUS_LISTEN] = {tcp_state_listen},
    [TCP_STATUS_SYN_SENT] = {tcp_state_drop},
    [TCP_STATUS_SYN_RECV] = {tcp_state_syn_recv},
    [TCP_STATUS_ESTABLISHED] = {tcp_state_established},
    [TCP_STATUS_CLOSE_WAIT] = {tcp_state_drop},
    [TCP_STATUS_LAST_ACK] = {tcp_state_drop},
    [TCP_STATUS_TIME_WAIT] = {tcp_state_drop},
    [TCP_STATUS_CLOSING] = {tcp_state_drop},
    [TCP_STATUS_FIN_WAIT_1] = {tcp_state_drop},
    [TCP_STATUS_FIN_WAIT_2] = {tcp_state_drop},
};

int tcp_ingress(struct rte_mbuf *mbuf) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *iphdr = tcp_ipv4_header(mbuf);
        struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(iphdr + 1);

        LOG_INFO("tcp rx " IP_FMT ":%u -> " IP_FMT ":%u flags=%s seq=%u ack=%u",
                 IP_ARG(iphdr->src_addr), rte_be_to_cpu_16(tcp_hdr->src_port),
                 IP_ARG(iphdr->dst_addr), rte_be_to_cpu_16(tcp_hdr->dst_port),
                 tcp_flags_str(tcp_hdr->tcp_flags), ntohl(tcp_hdr->sent_seq),
                 ntohl(tcp_hdr->recv_ack));

        arp_table_add(iphdr->src_addr, eth->src_addr.addr_bytes);

        /* TODO: only accept a SYN for a port a socket is actually listening on.
         * Today every inbound SYN to any local port creates a new socket, so
         * there is no real "listen" gate. */
        /* TODO: parse TCP options (MSS, window scale, SACK, timestamps) from
         * the SYN; opt_len is currently always treated as 0 on the receive
         * path. */
        /* TODO: generate/accept RST for segments that match no connection or
         * that arrive in an invalid state; RST is currently ignored entirely.
         */
        struct nsock *sk =
            tcp_stream_search(iphdr->src_addr, iphdr->dst_addr,
                              tcp_hdr->src_port, tcp_hdr->dst_port);
        if (sk == NULL)
                sk = tcp_stream_create(iphdr->src_addr, iphdr->dst_addr,
                                       tcp_hdr->src_port, tcp_hdr->dst_port);

        TCP_STATUS st = sk->u.tcp.status;
        int delivered = 0;
        if (st < TCP_STATUS_MAX && tcp_state_ops[st].handle != NULL)
                delivered = tcp_state_ops[st].handle(sk, tcp_hdr, mbuf);

        if (!delivered)
                rte_pktmbuf_free(mbuf);
        return 0;
}

static struct rte_mbuf *tcp_build_pkt(struct rte_mempool *mp, uint32_t src_ip,
                                      uint32_t dst_ip, uint8_t *dst_mac,
                                      struct tcp_fragment *f) {
        const size_t opt_bytes = (size_t)f->opt_len * sizeof(uint32_t);
        const size_t l4_len =
            sizeof(struct rte_tcp_hdr) + opt_bytes + f->payload_len;

        void *l4 = NULL;
        struct rte_mbuf *mbuf = eth_ipv4_build(mp, dst_mac, src_ip, dst_ip,
                                               IPPROTO_TCP, l4_len, &l4);
        if (mbuf == NULL) {
                LOG_ERROR("eth_ipv4_build failed");
                return NULL;
        }

        struct rte_ipv4_hdr *ip = tcp_ipv4_header(mbuf);
        struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)l4;
        tcp->src_port = f->src_port;
        tcp->dst_port = f->dst_port;
        tcp->sent_seq = htonl(f->sent_seq);
        tcp->recv_ack = htonl(f->recv_ack);
        tcp->data_off = f->data_off;
        tcp->tcp_flags = f->tcp_flags;
        tcp->rx_win = htons(f->rx_win);
        tcp->tcp_urp = f->tcp_urp;
        tcp->cksum = 0;
        tcp->cksum = rte_ipv4_udptcp_cksum(ip, tcp);

        if (f->payload_len > 0)
                rte_memcpy((uint8_t *)tcp + sizeof(struct rte_tcp_hdr) +
                               opt_bytes,
                           f->payload, f->payload_len);
        return mbuf;
}

int tcp_tx_flush(struct nsock *sk, struct rte_mempool *mp) {
        if (sk->u.tcp.status != TCP_STATUS_SYN_RECV &&
            sk->u.tcp.status != TCP_STATUS_ESTABLISHED)
                return 0;

        struct tcp_fragment *f = NULL;
        if (rte_ring_sc_dequeue(sk->send_buf, (void **)&f) < 0)
                return 0;

        uint32_t peer_ip = sk->u.tcp.remote_ip;
        uint8_t *dst_mac = arp_lookup(peer_ip);
        if (dst_mac == NULL) {
                LOG_INFO("tcp tx wait ARP for " IP_FMT
                         " flags=%s seq=%u ack=%u",
                         IP_ARG(peer_ip), tcp_flags_str(f->tcp_flags),
                         f->sent_seq, f->recv_ack);
                struct rte_mbuf *arp =
                    arp_build_pkt(mp, RTE_ARP_OP_REQUEST, g_broadcast_mac,
                                  g_net.local_ip, peer_ip);
                if (arp == NULL)
                        rte_exit(EXIT_FAILURE, "arp_build_pkt failed\n");
                struct inout_ring *ring = ring_instance();
                rte_ring_mp_enqueue_burst(ring->out, (void **)&arp, 1, NULL);
                /* send_buf is multi-producer (worker + app lcore tcp_send). */
                rte_ring_mp_enqueue(sk->send_buf, f);
                return 0;
        }

        struct rte_mbuf *tcp_buf =
            tcp_build_pkt(mp, g_net.local_ip, peer_ip, dst_mac, f);
        if (tcp_buf == NULL) {
                rte_free(f);
                return 0;
        }

        struct inout_ring *ring = ring_instance();
        rte_ring_mp_enqueue_burst(ring->out, (void **)&tcp_buf, 1, NULL);
        /* TODO: keep the just-sent fragment on a retransmission queue with an
         * RTO timer until the matching ACK arrives; on timeout, resend it.
         * Today the fragment is freed immediately and never retransmitted, so a
         * lost SYN+ACK or data segment stalls the connection forever. */

        if ((f->tcp_flags & (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG)) ==
            (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG)) {
                LOG_INFO("tcp handshake [2/3] SYN+ACK tx " IP_FMT
                         ":%u -> " IP_FMT ":%u seq=%u ack=%u",
                         IP_ARG(g_net.local_ip), rte_be_to_cpu_16(f->src_port),
                         IP_ARG(peer_ip), rte_be_to_cpu_16(f->dst_port),
                         f->sent_seq, f->recv_ack);
        } else {
                LOG_INFO("tcp tx " IP_FMT ":%u -> " IP_FMT
                         ":%u flags=%s seq=%u ack=%u len=%zu",
                         IP_ARG(g_net.local_ip), rte_be_to_cpu_16(f->src_port),
                         IP_ARG(peer_ip), rte_be_to_cpu_16(f->dst_port),
                         tcp_flags_str(f->tcp_flags), f->sent_seq, f->recv_ack,
                         f->payload_len);
        }

        if (f->payload)
                rte_free(f->payload);
        rte_free(f);
        return 0;
}

ssize_t tcp_send(struct nsock *sk, const void *buf, size_t len,
                 __attribute__((unused)) const struct sockaddr *dest,
                 __attribute__((unused)) socklen_t addrlen) {
        if (sk->u.tcp.status != TCP_STATUS_ESTABLISHED) {
                LOG_ERROR("tcp_send: fd=%d not established (status=%s)", sk->fd,
                          tcp_status_str(sk->u.tcp.status));
                return -1;
        }
        if (len == 0)
                return 0;

        /* TODO: implement send-side flow control: do not enqueue more than the
         * peer's advertised window allows, and respect a per-socket send
         * buffer limit. Today tcp_send queues unbounded data regardless of the
         * peer window, and there is no backpressure toward the app. */
        /* TODO: split payloads larger than the negotiated MSS into multiple
         * segments; a single oversized fragment is currently encoded as one
         * frame. */
        struct tcp_fragment *f = tcp_fragment_alloc();
        f->src_port = sk->local_port;
        f->dst_port = sk->u.tcp.remote_port;
        f->sent_seq = sk->u.tcp.sent_seq;
        f->recv_ack = sk->u.tcp.recv_ack;
        f->tcp_flags = RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG;
        f->data_off = (5 << 4);
        f->rx_win = TCP_INITIAL_WINDOW_SIZE;
        f->payload_len = len;
        f->payload = rte_malloc("tcp_payload", len, 0);
        if (f->payload == NULL) {
                rte_free(f);
                LOG_ERROR("tcp_send: payload alloc failed");
                return -1;
        }
        rte_memcpy(f->payload, buf, len);

        if (rte_ring_mp_enqueue(sk->send_buf, f) != 0) {
                LOG_ERROR("tcp_send: send_buf full for fd=%d", sk->fd);
                rte_free(f->payload);
                rte_free(f);
                return -1;
        }
        sk->u.tcp.sent_seq += (uint32_t)len;

        LOG_INFO("tcp_send fd=%d " IP_FMT ":%u -> " IP_FMT
                 ":%u len=%zu data=%.*s",
                 sk->fd, IP_ARG(sk->local_ip), rte_be_to_cpu_16(sk->local_port),
                 IP_ARG(sk->u.tcp.remote_ip),
                 rte_be_to_cpu_16(sk->u.tcp.remote_port), len, (int)len,
                 (const char *)buf);
        return (ssize_t)len;
}

ssize_t tcp_recv(struct nsock *sk, void *buf, size_t len,
                 struct sockaddr *src_addr,
                 __attribute__((unused)) socklen_t *addrlen) {
        struct rte_mbuf *mbuf;
        int nb = -1;
        pthread_mutex_lock(&sk->mutex);
        while ((nb = rte_ring_sc_dequeue(sk->recv_buf, (void **)&mbuf)) != 0)
                pthread_cond_wait(&sk->cond, &sk->mutex);
        pthread_mutex_unlock(&sk->mutex);

        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
        struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip + 1);

        if (src_addr) {
                struct sockaddr_in *sin = (struct sockaddr_in *)src_addr;
                sin->sin_family = AF_INET;
                sin->sin_port = tcp->src_port;
                sin->sin_addr.s_addr = ip->src_addr;
        }

        uint8_t hdrlen = (tcp->data_off >> 4) * 4;
        size_t payload_len = rte_be_to_cpu_16(ip->total_length) -
                             sizeof(struct rte_ipv4_hdr) - hdrlen;
        const void *data = (const void *)((uint8_t *)tcp + hdrlen);

        size_t n = len < payload_len ? len : payload_len;
        rte_memcpy(buf, data, n);
        rte_pktmbuf_free(mbuf);

        /* TODO: byte-stream reassembly; a short read currently drops the
         * remainder of this segment. */
        if (n < payload_len)
                LOG_WARN("tcp_recv fd=%d short read: %zu of %zu bytes dropped",
                         sk->fd, n, payload_len);
        return (ssize_t)n;
}

static void tcp_drain_send(struct nsock *sk) {
        struct tcp_fragment *f;
        while (rte_ring_sc_dequeue(sk->send_buf, (void **)&f) == 0) {
                if (f->payload)
                        rte_free(f->payload);
                rte_free(f);
        }
}

static void tcp_drain_recv(struct nsock *sk) {
        struct rte_mbuf *m;
        while (rte_ring_sc_dequeue(sk->recv_buf, (void **)&m) == 0)
                rte_pktmbuf_free(m);
}

int tcp_close(struct nsock *sk) {
        LOG_INFO("tcp_close fd=%d status=%s", sk->fd,
                 tcp_status_str(sk->u.tcp.status));
        /* TODO: perform a real FIN exchange (FIN_WAIT_1 -> FIN_WAIT_2 ->
         * TIME_WAIT / CLOSE_WAIT -> LAST_ACK) instead of dropping the queues.
         * This is the teardown counterpart of the tcp_state_drop stubs. */
        tcp_drain_send(sk);
        tcp_drain_recv(sk);
        nsock_free(sk);
        return 0;
}

const struct sock_ops tcp_ops = {
    .name = "tcp",
    .protocol = IPPROTO_TCP,
    .ingress = tcp_ingress,
    .tx_flush = tcp_tx_flush,
    .send = tcp_send,
    .recv = tcp_recv,
    .close = tcp_close,
    /* TODO: implement active/passive open so TCP can be used as a client and
     * as a real server with a backlog. Until then nconnect/nlisten/naccept
     * return -1 (see socket.c). */
    .connect = NULL,
    .listen = NULL,
    .accept = NULL,
};
