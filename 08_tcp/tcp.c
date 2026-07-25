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
#include <rte_bitops.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf_core.h>
#include <rte_ring.h>
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

static struct nsock *tcp_listener_lookup(uint32_t local_ip,
                                         uint16_t local_port) {
        struct nsock *sk;
        for (sk = g_sock_list; sk != NULL; sk = sk->next) {
                if (sk->protocol != IPPROTO_TCP)
                        continue;
                if (sk->u.tcp.status != TCP_STATUS_LISTEN)
                        continue;
                if (sk->local_ip == local_ip && sk->local_port == local_port)
                        return sk;
        }
        return NULL;
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

/*
 * Create a passive-open child TCB. No fd yet: the peer can flood SYNs and we
 * still need TCBs + rings, but userspace fds are only handed out in tcp_accept
 * after the 3WHS completes.
 */
struct nsock *tcp_stream_create(uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port) {
        struct nsock *sk = nsock_alloc(-1, IPPROTO_TCP);
        if (sk == NULL) {
                LOG_ERROR("tcp_stream_create: nsock_alloc failed");
                return NULL;
        }

        sk->local_ip = local_ip;
        sk->local_port = local_port;
        sk->u.tcp.remote_ip = remote_ip;
        sk->u.tcp.remote_port = remote_port;
        sk->u.tcp.status = TCP_STATUS_SYN_RECV;
        sk->u.tcp.listener = NULL;
        sk->u.tcp.sent_seq = tcp_next_isn();
        sk->u.tcp.recv_ack = 0;

        LOG_INFO("tcp stream create " IP_FMT ":%u -> " IP_FMT
                 ":%u isn=%u status=%s (fd deferred until accept)",
                 IP_ARG(remote_ip), rte_be_to_cpu_16(remote_port),
                 IP_ARG(local_ip), rte_be_to_cpu_16(local_port),
                 sk->u.tcp.sent_seq, tcp_status_str(sk->u.tcp.status));
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

static void tcp_drain_send(struct nsock *sk);
static void tcp_drain_recv(struct nsock *sk);

static int tcp_enqueue_fragment(struct nsock *sk, struct tcp_fragment *f) {
        if (rte_ring_mp_enqueue(sk->send_buf, f) != 0) {
                LOG_ERROR("tcp send_buf full for fd=%d flags=0x%02x", sk->fd,
                          f->tcp_flags);
                if (f->payload)
                        rte_free(f->payload);
                rte_free(f);
                return -1;
        }
        return 0;
}

/*
 * Build a header-only (or payload-bearing) outbound fragment for @p sk.
 * Ports and sequence numbers follow tcp_fragment conventions: ports network
 * order, sent_seq/recv_ack/rx_win host order.
 */
static struct tcp_fragment *tcp_make_fragment(struct nsock *sk, uint8_t flags,
                                              uint32_t sent_seq,
                                              uint32_t recv_ack) {
        struct tcp_fragment *f = tcp_fragment_alloc();
        f->src_port = sk->local_port;
        f->dst_port = sk->u.tcp.remote_port;
        f->sent_seq = sent_seq;
        f->recv_ack = recv_ack;
        f->tcp_flags = flags;
        f->data_off = (5 << 4);
        f->rx_win = TCP_INITIAL_WINDOW_SIZE;
        return f;
}

/*
 * Passive open step 1: new SYN for @p listener.
 * Creates a SYN_RECV child (no fd), queues SYN+ACK on the child send_buf.
 * Does NOT enqueue onto accept_queue -- that happens only after the final ACK.
 */
static int tcp_state_listen(struct nsock *listener, struct rte_tcp_hdr *hdr,
                            uint32_t remote_ip, uint16_t remote_port) {
        if (!(hdr->tcp_flags & RTE_TCP_SYN_FLAG))
                return 0;
        /* Ignore SYN+ACK (active-open reply); only bare SYN opens a child. */
        if (hdr->tcp_flags & RTE_TCP_ACK_FLAG)
                return 0;
        if (listener->u.tcp.status != TCP_STATUS_LISTEN)
                return 0;
        if (listener->u.tcp.accept_queue == NULL)
                return 0;

        /*
         * Backlog gates both incomplete handshakes and completed-but-unaccepted
         * connections. Without this, a SYN flood allocates unbounded TCBs.
         * accept_queue is sized (in tcp_listen) to hold @backlog entries.
         */
        unsigned int accepted = rte_ring_count(listener->u.tcp.accept_queue);
        if (listener->u.tcp.syn_pending + accepted >= listener->u.tcp.backlog) {
                LOG_WARN("tcp backlog full listen_fd=%d syn_pending=%u "
                         "accepted=%u backlog=%u; drop SYN from " IP_FMT ":%u",
                         listener->fd, listener->u.tcp.syn_pending, accepted,
                         listener->u.tcp.backlog, IP_ARG(remote_ip),
                         rte_be_to_cpu_16(remote_port));
                return 0;
        }

        struct nsock *child = tcp_stream_create(
            remote_ip, listener->local_ip, remote_port, listener->local_port);
        if (child == NULL) {
                LOG_ERROR("tcp stream create failed listen_fd=%d peer " IP_FMT
                          ":%u",
                          listener->fd, IP_ARG(remote_ip),
                          rte_be_to_cpu_16(remote_port));
                return 0;
        }
        /* Used by tcp_state_syn_recv to wake naccept on the parent. */
        child->u.tcp.listener = listener;
        listener->u.tcp.syn_pending++;

        struct tcp_fragment *f = tcp_fragment_alloc();
        f->src_port = child->local_port;
        f->dst_port = child->u.tcp.remote_port;
        f->sent_seq = child->u.tcp.sent_seq;
        f->recv_ack = ntohl(hdr->sent_seq) + 1;
        f->tcp_flags = RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG;
        f->data_off = (5 << 4);
        f->rx_win = TCP_INITIAL_WINDOW_SIZE;
        child->u.tcp.recv_ack = f->recv_ack;
        rte_ring_mp_enqueue(child->send_buf, f);

        LOG_INFO("tcp handshake [1/3] SYN rx " IP_FMT ":%u -> " IP_FMT
                 ":%u seq=%u; reply SYN+ACK seq=%u ack=%u syn_pending=%u",
                 IP_ARG(remote_ip), rte_be_to_cpu_16(remote_port),
                 IP_ARG(listener->local_ip),
                 rte_be_to_cpu_16(listener->local_port), ntohl(hdr->sent_seq),
                 child->u.tcp.sent_seq, child->u.tcp.recv_ack,
                 listener->u.tcp.syn_pending);
        return 0;
}

static int tcp_state_syn_recv(struct nsock *sk, struct rte_tcp_hdr *hdr,
                              struct rte_mbuf *mbuf) {
        (void)mbuf;
        if (sk->u.tcp.status != TCP_STATUS_SYN_RECV)
                return 0;

        /*
         * Peer lost our SYN+ACK and retransmitted SYN: resend SYN+ACK with the
         * same ISN / ack. Do not allocate another child (4-tuple already maps
         * here via tcp_stream_search).
         */
        if ((hdr->tcp_flags & RTE_TCP_SYN_FLAG) &&
            !(hdr->tcp_flags & RTE_TCP_ACK_FLAG)) {
                struct tcp_fragment *f =
                    tcp_make_fragment(sk, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG,
                                      sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
                if (tcp_enqueue_fragment(sk, f) == 0)
                        LOG_INFO("tcp SYN_RECV retransmit SYN+ACK " IP_FMT
                                 ":%u seq=%u ack=%u",
                                 IP_ARG(sk->u.tcp.remote_ip),
                                 rte_be_to_cpu_16(sk->u.tcp.remote_port),
                                 sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
                return 0;
        }

        if (!(hdr->tcp_flags & RTE_TCP_ACK_FLAG)) {
                LOG_DEBUG("tcp SYN_RECV ignore flags=%s from " IP_FMT ":%u",
                          tcp_flags_str(hdr->tcp_flags),
                          IP_ARG(sk->u.tcp.remote_ip),
                          rte_be_to_cpu_16(sk->u.tcp.remote_port));
                return 0;
        }

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

        /*
         * Handshake done: move child from syn_pending into accept_queue so
         * a blocked naccept can return. rte_ring_mp_enqueue returns 0 on
         * success.
         */
        struct nsock *listener = sk->u.tcp.listener;
        if (listener != NULL && listener->u.tcp.accept_queue != NULL) {
                if (listener->u.tcp.syn_pending > 0)
                        listener->u.tcp.syn_pending--;
                if (rte_ring_mp_enqueue(listener->u.tcp.accept_queue, sk) ==
                    0) {
                        LOG_INFO("tcp accept_queue enqueue peer " IP_FMT
                                 ":%u listen_fd=%d",
                                 IP_ARG(sk->u.tcp.remote_ip),
                                 rte_be_to_cpu_16(sk->u.tcp.remote_port),
                                 listener->fd);
                        pthread_mutex_lock(&listener->mutex);
                        pthread_cond_signal(&listener->cond);
                        pthread_mutex_unlock(&listener->mutex);
                } else {
                        /* TODO: send RST to the peer before freeing. */
                        LOG_ERROR(
                            "tcp accept_queue full listen_fd=%d peer " IP_FMT
                            ":%u; free child TCB",
                            listener->fd, IP_ARG(sk->u.tcp.remote_ip),
                            rte_be_to_cpu_16(sk->u.tcp.remote_port));
                        tcp_drain_send(sk);
                        tcp_drain_recv(sk);
                        nsock_free(sk);
                }
        }
        return 0;
}

static int tcp_state_established(struct nsock *sk, struct rte_tcp_hdr *hdr,
                                 struct rte_mbuf *mbuf) {
        uint8_t hdrlen = (hdr->data_off >> 4) * 4;
        uint16_t payload_len =
            rte_be_to_cpu_16(tcp_ipv4_header(mbuf)->total_length) -
            sizeof(struct rte_ipv4_hdr) - hdrlen;

        int has_fin = !!(hdr->tcp_flags & RTE_TCP_FIN_FLAG);

        if (payload_len == 0 && !has_fin)
                return 0; /* pure ACK: caller frees mbuf */

        /* SYN is unexpected once established; ignore for now. */
        if (hdr->tcp_flags & RTE_TCP_SYN_FLAG)
                return 0;

        /* In-order only for now. TODO: buffer/reassemble out-of-order and
         * overlapping segments (receive window / reassembly queue). */
        uint32_t seq = ntohl(hdr->sent_seq);
        if (seq != sk->u.tcp.recv_ack) {
                LOG_DEBUG(
                    "tcp ESTABLISHED drop ooo/dup seq=%u expect=%u len=%u", seq,
                    sk->u.tcp.recv_ack, payload_len);
                return 0;
        }

        /* In-order data: advance the next expected peer sequence number. */
        sk->u.tcp.recv_ack += payload_len;
        if (has_fin)
                sk->u.tcp.recv_ack += 1; // fin consumes one sequence number

        /* Pure ACK so the peer can retire its in-flight bytes. */
        struct tcp_fragment *ack_f = tcp_make_fragment(
            sk, RTE_TCP_ACK_FLAG, sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
        (void)tcp_enqueue_fragment(sk, ack_f);

        /* TODO: honor the peer's advertised rx_win (hdr->rx_win) before letting
         * tcp_send enqueue more data -- there is no flow control / sliding
         * window yet. */

        if (has_fin) {
                tcp_stream_set_status(sk, TCP_STATUS_CLOSE_WAIT);
        }

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
    /* LISTEN is handled in tcp_ingress via tcp_listener_lookup +
       tcp_state_listen. */
    [TCP_STATUS_LISTEN] = {tcp_state_drop},
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

        /* TODO: parse TCP options (MSS, window scale, SACK, timestamps) from
         * the SYN; opt_len is currently always treated as 0 on the receive
         * path. */
        /* TODO: generate/accept RST for segments that match no connection or
         * that arrive in an invalid state; RST is currently ignored entirely.
         */

        uint32_t remote_ip = iphdr->src_addr;
        uint32_t local_ip = iphdr->dst_addr;
        uint16_t remote_port = tcp_hdr->src_port;
        uint16_t local_port = tcp_hdr->dst_port;

        struct nsock *sk =
            tcp_stream_search(remote_ip, local_ip, remote_port, local_port);

        /* Existing TCB (including SYN_RECV children still awaiting final ACK).
         */
        if (sk != NULL) {
                int delivered = 0;
                TCP_STATUS st = sk->u.tcp.status;
                if (st < TCP_STATUS_MAX && tcp_state_ops[st].handle != NULL)
                        delivered = tcp_state_ops[st].handle(sk, tcp_hdr, mbuf);
                if (!delivered)
                        rte_pktmbuf_free(mbuf);
                return 0;
        }

        /* New connection: only a bare SYN on a listening port opens a child. */
        if ((tcp_hdr->tcp_flags & RTE_TCP_SYN_FLAG) &&
            !(tcp_hdr->tcp_flags & RTE_TCP_ACK_FLAG)) {
                struct nsock *listener =
                    tcp_listener_lookup(local_ip, local_port);
                if (listener != NULL) {
                        (void)tcp_state_listen(listener, tcp_hdr, remote_ip,
                                               remote_port);
                } else {
                        /* TODO: send RST for SYN to a closed port. */
                        LOG_DEBUG(
                            "tcp SYN to non-listening " IP_FMT
                            ":%u from " IP_FMT ":%u; drop",
                            IP_ARG(local_ip), rte_be_to_cpu_16(local_port),
                            IP_ARG(remote_ip), rte_be_to_cpu_16(remote_port));
                }
                rte_pktmbuf_free(mbuf);
                return 0;
        }

        /* TODO: RST -- segment matched neither a TCB nor a listener. */
        LOG_DEBUG("tcp drop unmatched " IP_FMT ":%u -> " IP_FMT ":%u flags=%s",
                  IP_ARG(remote_ip), rte_be_to_cpu_16(remote_port),
                  IP_ARG(local_ip), rte_be_to_cpu_16(local_port),
                  tcp_flags_str(tcp_hdr->tcp_flags));
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

        if (f->payload_len > 0)
                rte_memcpy((uint8_t *)tcp + sizeof(struct rte_tcp_hdr) +
                               opt_bytes,
                           f->payload, f->payload_len);

        tcp->cksum = rte_ipv4_udptcp_cksum(ip, tcp);
        return mbuf;
}

int tcp_tx_flush(struct nsock *sk, struct rte_mempool *mp) {
        /* States that may still have outbound segments on send_buf. */
        switch (sk->u.tcp.status) {
        case TCP_STATUS_SYN_RECV:
        case TCP_STATUS_ESTABLISHED:
        case TCP_STATUS_CLOSE_WAIT: /* ACK of peer FIN may still be queued */
        case TCP_STATUS_LAST_ACK:   /* our FIN after passive close */
        case TCP_STATUS_FIN_WAIT_1: /* our FIN after active close */
                break;
        default:
                return 0;
        }

        /* Drain send_buf so ACK-of-FIN + our FIN leave in the same pass. */
        for (;;) {
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
                        struct rte_mbuf *arp = arp_build_pkt(
                            mp, RTE_ARP_OP_REQUEST, g_broadcast_mac,
                            g_net.local_ip, peer_ip);
                        if (arp == NULL)
                                rte_exit(EXIT_FAILURE,
                                         "arp_build_pkt failed\n");
                        struct inout_ring *ring = ring_instance();
                        rte_ring_mp_enqueue_burst(ring->out, (void **)&arp, 1,
                                                  NULL);
                        /* send_buf is multi-producer (worker + app lcore). */
                        rte_ring_mp_enqueue(sk->send_buf, f);
                        return 0;
                }

                struct rte_mbuf *tcp_buf =
                    tcp_build_pkt(mp, g_net.local_ip, peer_ip, dst_mac, f);
                if (tcp_buf == NULL) {
                        rte_free(f);
                        continue;
                }

                struct inout_ring *ring = ring_instance();
                rte_ring_mp_enqueue_burst(ring->out, (void **)&tcp_buf, 1,
                                          NULL);
                /* TODO: keep the just-sent fragment on a retransmission queue
                 * with an RTO timer until the matching ACK arrives; on timeout,
                 * resend it. Today the fragment is freed immediately and never
                 * retransmitted, so a lost SYN+ACK or data segment stalls the
                 * connection forever. */

                if ((f->tcp_flags & (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG)) ==
                    (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG)) {
                        LOG_INFO("tcp handshake [2/3] SYN+ACK tx " IP_FMT
                                 ":%u -> " IP_FMT ":%u seq=%u ack=%u",
                                 IP_ARG(g_net.local_ip),
                                 rte_be_to_cpu_16(f->src_port), IP_ARG(peer_ip),
                                 rte_be_to_cpu_16(f->dst_port), f->sent_seq,
                                 f->recv_ack);
                } else {
                        LOG_INFO("tcp tx " IP_FMT ":%u -> " IP_FMT
                                 ":%u flags=%s seq=%u ack=%u len=%zu",
                                 IP_ARG(g_net.local_ip),
                                 rte_be_to_cpu_16(f->src_port), IP_ARG(peer_ip),
                                 rte_be_to_cpu_16(f->dst_port),
                                 tcp_flags_str(f->tcp_flags), f->sent_seq,
                                 f->recv_ack, f->payload_len);
                }

                if (f->payload)
                        rte_free(f->payload);
                rte_free(f);
        }
}

ssize_t tcp_send(struct nsock *sk, const void *buf, size_t len,
                 __attribute__((unused)) int flags) {
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
                 __attribute__((unused)) int flags) {
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

        uint8_t hdrlen = (tcp->data_off >> 4) * 4;
        size_t payload_len = rte_be_to_cpu_16(ip->total_length) -
                             sizeof(struct rte_ipv4_hdr) - hdrlen;
        uint8_t *data = (uint8_t *)tcp + hdrlen;

        size_t n = len < payload_len ? len : payload_len;
        rte_memcpy(buf, data, n);

        if (n < payload_len && n > 0) {
                /* Keep the unread tail so the next recv continues the stream.
                 */
                memmove(data, data + n, payload_len - n);
                ip->total_length =
                    rte_cpu_to_be_16((uint16_t)(sizeof(struct rte_ipv4_hdr) +
                                                hdrlen + (payload_len - n)));
                if (rte_ring_mp_enqueue(sk->recv_buf, mbuf) != 0) {
                        LOG_ERROR("tcp_recv: recv_buf full while truncating, "
                                  "dropping remainder");
                        rte_pktmbuf_free(mbuf);
                }
                return (ssize_t)n;
        }

        rte_pktmbuf_free(mbuf);
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

        /* Listener teardown: completed children in accept_queue + half-open. */
        if (sk->u.tcp.accept_queue != NULL) {
                struct nsock *child;
                while (rte_ring_sc_dequeue(sk->u.tcp.accept_queue,
                                           (void **)&child) == 0) {
                        tcp_drain_send(child);
                        tcp_drain_recv(child);
                        nsock_free(child);
                }
                rte_ring_free(sk->u.tcp.accept_queue);
                sk->u.tcp.accept_queue = NULL;
        }
        if (sk->u.tcp.status == TCP_STATUS_LISTEN) {
                struct nsock *cur = g_sock_list;
                while (cur != NULL) {
                        struct nsock *next = cur->next;
                        if (cur != sk && cur->protocol == IPPROTO_TCP &&
                            cur->u.tcp.listener == sk) {
                                LOG_INFO(
                                    "tcp_close: free orphan child peer " IP_FMT
                                    ":%u status=%s",
                                    IP_ARG(cur->u.tcp.remote_ip),
                                    rte_be_to_cpu_16(cur->u.tcp.remote_port),
                                    tcp_status_str(cur->u.tcp.status));
                                tcp_drain_send(cur);
                                tcp_drain_recv(cur);
                                nsock_free(cur);
                        }
                        cur = next;
                }
                sk->u.tcp.syn_pending = 0;
        }

        // Passive close
        if (sk->u.tcp.status == TCP_STATUS_CLOSE_WAIT) {
                struct tcp_fragment *fin_f =
                    tcp_make_fragment(sk, RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG,
                                      sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
                if (tcp_enqueue_fragment(sk, fin_f) == 0)
                        sk->u.tcp.sent_seq += 1;

                tcp_stream_set_status(sk, TCP_STATUS_LAST_ACK);
                tcp_drain_recv(sk);
                return 0;
        }

        // Active close
        // TODO: implement active close

        tcp_drain_send(sk);
        tcp_drain_recv(sk);
        nsock_free(sk);
        return 0;
}

int tcp_listen(struct nsock *sk, int backlog) {
        /* Listener has no peer; children carry the remote 4-tuple half. */
        sk->u.tcp.status = TCP_STATUS_LISTEN;
        sk->u.tcp.remote_ip = 0;
        sk->u.tcp.remote_port = 0;
        sk->u.tcp.syn_pending = 0;
        sk->u.tcp.listener = NULL;

        if (backlog < 1)
                backlog = 1;
        sk->u.tcp.backlog = (uint32_t)backlog;

        /*
         * rte_ring usable capacity is size-1 and size must be a power of 2.
         * Size the ring so it can hold @backlog completed connections.
         */
        unsigned int ring_sz = rte_align32pow2((uint32_t)backlog + 1u);
        if (ring_sz < 2)
                ring_sz = 2;

        char name[32];
        snprintf(name, sizeof(name), "tcp_accept_%d", sk->fd);
        sk->u.tcp.accept_queue =
            rte_ring_create(name, ring_sz, rte_socket_id(), 0);
        if (sk->u.tcp.accept_queue == NULL) {
                LOG_ERROR("tcp_listen: accept_queue create failed fd=%d "
                          "backlog=%d ring_sz=%u",
                          sk->fd, backlog, ring_sz);
                return -1;
        }
        LOG_INFO("tcp_listen fd=%d " IP_FMT ":%u backlog=%d ring_sz=%u", sk->fd,
                 IP_ARG(sk->local_ip), rte_be_to_cpu_16(sk->local_port),
                 backlog, ring_sz);
        return 0;
}

int tcp_accept(struct nsock *sk, struct sockaddr *addr,
               __attribute__((unused)) socklen_t *addrlen) {
        if (sk->u.tcp.status != TCP_STATUS_LISTEN) {
                LOG_ERROR("tcp_accept: fd=%d not listening", sk->fd);
                return -1;
        }
        if (sk->u.tcp.accept_queue == NULL) {
                LOG_ERROR("tcp_accept: fd=%d has no accept_queue", sk->fd);
                return -1;
        }

        struct nsock *child;
        pthread_mutex_lock(&sk->mutex);
        while (rte_ring_sc_dequeue(sk->u.tcp.accept_queue, (void **)&child) <
               0) {
                /* Block until tcp_state_syn_recv signals a completed handshake.
                 */
                pthread_cond_wait(&sk->cond, &sk->mutex);
        }
        pthread_mutex_unlock(&sk->mutex);

        /*
         * First time this TCB becomes visible to the application: allocate
         * the fd here, not on SYN. That way a SYN flood cannot burn fds.
         */
        int fd = fd_alloc();
        if (fd < 0) {
                LOG_ERROR(
                    "tcp_accept: fd table full; requeue child peer " IP_FMT
                    ":%u",
                    IP_ARG(child->u.tcp.remote_ip),
                    rte_be_to_cpu_16(child->u.tcp.remote_port));
                /* Put it back so a later accept can retry. */
                if (rte_ring_mp_enqueue(sk->u.tcp.accept_queue, child) != 0) {
                        LOG_ERROR("tcp_accept: requeue failed; dropping child");
                        tcp_drain_send(child);
                        tcp_drain_recv(child);
                        nsock_free(child);
                } else {
                        pthread_mutex_lock(&sk->mutex);
                        pthread_cond_signal(&sk->cond);
                        pthread_mutex_unlock(&sk->mutex);
                }
                return -1;
        }
        child->fd = fd;

        if (addr) {
                struct sockaddr_in *sin = (struct sockaddr_in *)addr;
                sin->sin_family = AF_INET;
                sin->sin_port = child->u.tcp.remote_port;
                sin->sin_addr.s_addr = child->u.tcp.remote_ip;
        }

        LOG_INFO("tcp_accept listen_fd=%d -> child_fd=%d peer " IP_FMT ":%u",
                 sk->fd, child->fd, IP_ARG(child->u.tcp.remote_ip),
                 rte_be_to_cpu_16(child->u.tcp.remote_port));
        return child->fd;
}

const struct sock_ops tcp_ops = {
    .name = "tcp",
    .protocol = IPPROTO_TCP,
    .ingress = tcp_ingress,
    .tx_flush = tcp_tx_flush,
    .send = tcp_send,
    .recv = tcp_recv,
    .sendto = NULL,
    .recvfrom = NULL,
    .close = tcp_close,
    /* TODO: implement active open (tcp_ops.connect / SYN_SENT path).
     * Passive open (listen/accept) is implemented. */
    .connect = NULL,
    .listen = tcp_listen,
    .accept = tcp_accept,
};
