/**
 * @file tcp.c
 * @author your name (you@domain.com)
 * @brief
 * @version 0.1
 * @date 2026-07-24
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "tcp.h"
#include "arp.h"
#include "config.h"
#include "list.h"
#include "log.h"
#include "net_context.h"
#include "ring.h"

#include <netinet/in.h>
#include <netinet/ip.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_ip4.h>
#include <rte_lcore.h>
#include <rte_mbuf_core.h>
#include <rte_tcp.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#define TCP_INITIAL_WINDOW_SIZE 14600 // 1460( default mss) * 10

struct tcp_table *t_instance = NULL;

static const char *tcp_status_str(TCP_STATUS status) {
        switch (status) {
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

struct tcp_table *tcp_table_instance(void) {
        if (t_instance == NULL) {
                t_instance =
                    rte_malloc("tcp_table", sizeof(struct tcp_table), 0);
                if (t_instance == NULL) {
                        rte_exit(EXIT_FAILURE,
                                 "rte_malloc(tcp_table) failed\n");
                }
                t_instance->count = 0;
                t_instance->tcb_set = NULL;
        }
        return t_instance;
}

struct tcp_stream *tcp_stream_search(uint32_t src_ip, uint32_t dst_ip,
                                     uint16_t src_port, uint16_t dst_port,
                                     __attribute__((unused)) uint8_t proto) {
        struct tcp_table *table = tcp_table_instance();

        struct tcp_stream *stream = NULL;
        for (stream = table->tcb_set; stream != NULL; stream = stream->next) {
                if (stream->src_ip == src_ip && stream->dst_ip == dst_ip &&
                    stream->src_port == src_port &&
                    stream->dst_port == dst_port) {
                        return stream;
                }
        }
        return NULL;
}

struct tcp_stream *tcp_stream_create(uint32_t src_ip, uint32_t dst_ip,
                                     uint16_t src_port, uint16_t dst_port,
                                     __attribute__((unused)) uint8_t proto) {
        struct tcp_stream *stream =
            rte_malloc("tcp_stream", sizeof(struct tcp_stream), 0);
        if (stream == NULL) {
                rte_exit(EXIT_FAILURE, "rte_malloc(tcp_stream) failed\n");
        }
        stream->src_ip = src_ip;
        stream->dst_ip = dst_ip;
        stream->src_port = src_port;
        stream->dst_port = dst_port;
        stream->proto = IPPROTO_TCP;

        // TODO: Currently, only the scenario where DPDK is used as the server
        // is implemented, so the state shown here is the initial state of the
        // server. In a real implementation, additional parameters are needed to
        // determine whether it is the client or the server.
        stream->status = TCP_STATUS_LISTEN;

        /* Ring names must be unique within the process. */
        char recv_name[32], send_name[32];
        static uint32_t ring_id;
        uint32_t id = ring_id++;
        snprintf(recv_name, sizeof(recv_name), "tcp_recv_%u", id);
        snprintf(send_name, sizeof(send_name), "tcp_send_%u", id);
        stream->recv_buf =
            rte_ring_create(recv_name, RING_SIZE, rte_socket_id(), 0);
        stream->send_buf =
            rte_ring_create(send_name, RING_SIZE, rte_socket_id(), 0);
        if (stream->recv_buf == NULL || stream->send_buf == NULL) {
                rte_exit(EXIT_FAILURE, "rte_ring_create(tcp) failed\n");
        }

        // randomize the first sequence number ( 0 - 2^32 -1)
        uint32_t random_seed = time(NULL);
        stream->sent_seq = rand_r(&random_seed) % (UINT32_MAX);

        rte_memcpy(stream->local_mac, g_net.local_mac, RTE_ETHER_ADDR_LEN);

        struct tcp_table *table = tcp_table_instance();
        LL_ADD(stream, table->tcb_set);
        table->count++;

        LOG_INFO("tcp stream create " IP_FMT ":%u -> " IP_FMT
                 ":%u isn=%u status=%s (tcb=%d)",
                 IP_ARG(src_ip), rte_be_to_cpu_16(src_port), IP_ARG(dst_ip),
                 rte_be_to_cpu_16(dst_port), stream->sent_seq,
                 tcp_status_str(stream->status), table->count);

        return stream;
}

int tcp_stream_handle_listen(struct tcp_stream *stream,
                             struct rte_tcp_hdr *tcphdr) {
        // TODO: Currently, only one scenario is being handled.
        if (tcphdr->tcp_flags & RTE_TCP_SYN_FLAG) {
                // check if the stream is first seen
                if (stream->status == TCP_STATUS_LISTEN) {
                        struct tcp_fragment *fragment = rte_malloc(
                            "tcp_fragment", sizeof(struct tcp_fragment), 0);
                        if (fragment == NULL) {
                                rte_exit(EXIT_FAILURE,
                                         "rte_malloc(tcp_fragment) failed\n");
                        }
                        fragment->src_port = tcphdr->dst_port;
                        fragment->dst_port = tcphdr->src_port;

                        fragment->sent_seq = stream->sent_seq;
                        fragment->recv_ack = ntohl(tcphdr->sent_seq) + 1;

                        fragment->tcp_flags =
                            (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG);
                        /* data_off: upper 4 bits = header len in 32-bit words
                         */
                        fragment->data_off = (5 << 4);
                        fragment->rx_win = TCP_INITIAL_WINDOW_SIZE;
                        fragment->tcp_urp = 0;
                        fragment->opt_len = 0;

                        fragment->payload = NULL;
                        fragment->payload_len = 0;

                        LOG_INFO("tcp handshake [1/3] SYN rx " IP_FMT
                                 ":%u -> " IP_FMT
                                 ":%u seq=%u; reply SYN+ACK seq=%u ack=%u",
                                 IP_ARG(stream->src_ip),
                                 rte_be_to_cpu_16(stream->src_port),
                                 IP_ARG(stream->dst_ip),
                                 rte_be_to_cpu_16(stream->dst_port),
                                 ntohl(tcphdr->sent_seq), fragment->sent_seq,
                                 fragment->recv_ack);

                        rte_ring_sp_enqueue(stream->send_buf, fragment);

                        stream->status = TCP_STATUS_SYN_RECV;
                        LOG_INFO("tcp status %s -> %s " IP_FMT ":%u <-> " IP_FMT
                                 ":%u",
                                 tcp_status_str(TCP_STATUS_LISTEN),
                                 tcp_status_str(stream->status),
                                 IP_ARG(stream->src_ip),
                                 rte_be_to_cpu_16(stream->src_port),
                                 IP_ARG(stream->dst_ip),
                                 rte_be_to_cpu_16(stream->dst_port));
                }
        } else {
                LOG_DEBUG("tcp LISTEN ignore flags=%s from " IP_FMT ":%u",
                          tcp_flags_str(tcphdr->tcp_flags),
                          IP_ARG(stream->src_ip),
                          rte_be_to_cpu_16(stream->src_port));
        }

        return 0;
}

int tcp_stream_handle_syn_recv(struct tcp_stream *stream,
                               struct rte_tcp_hdr *tcp_hdr) {
        if (tcp_hdr->tcp_flags & RTE_TCP_ACK_FLAG) {
                if (stream->status == TCP_STATUS_SYN_RECV) {
                        uint32_t acknum = ntohl(tcp_hdr->recv_ack);
                        uint32_t expect_ack = stream->sent_seq + 1;

                        if (acknum == expect_ack) {
                                LOG_INFO("tcp handshake [3/3] ACK rx " IP_FMT
                                         ":%u -> " IP_FMT
                                         ":%u seq=%u ack=%u (expect=%u)",
                                         IP_ARG(stream->src_ip),
                                         rte_be_to_cpu_16(stream->src_port),
                                         IP_ARG(stream->dst_ip),
                                         rte_be_to_cpu_16(stream->dst_port),
                                         ntohl(tcp_hdr->sent_seq), acknum,
                                         expect_ack);
                        } else {
                                LOG_WARN("tcp handshake ACK mismatch " IP_FMT
                                         ":%u -> " IP_FMT
                                         ":%u ack=%u expect=%u",
                                         IP_ARG(stream->src_ip),
                                         rte_be_to_cpu_16(stream->src_port),
                                         IP_ARG(stream->dst_ip),
                                         rte_be_to_cpu_16(stream->dst_port),
                                         acknum, expect_ack);
                        }

                        stream->status = TCP_STATUS_ESTABLISHED;
                        LOG_INFO("tcp handshake done, status %s -> %s " IP_FMT
                                 ":%u <-> " IP_FMT ":%u",
                                 tcp_status_str(TCP_STATUS_SYN_RECV),
                                 tcp_status_str(stream->status),
                                 IP_ARG(stream->src_ip),
                                 rte_be_to_cpu_16(stream->src_port),
                                 IP_ARG(stream->dst_ip),
                                 rte_be_to_cpu_16(stream->dst_port));
                }
        } else {
                LOG_DEBUG("tcp SYN_RECV ignore flags=%s from " IP_FMT ":%u",
                          tcp_flags_str(tcp_hdr->tcp_flags),
                          IP_ARG(stream->src_ip),
                          rte_be_to_cpu_16(stream->src_port));
        }

        return 0;
}

static struct rte_ipv4_hdr *tcp_ipv4_header(struct rte_mbuf *mbuf) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        return (struct rte_ipv4_hdr *)(eth + 1);
}

/** packet handler for TCP */
int tcp_handle(struct rte_mbuf *mbuf) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *iphdr = tcp_ipv4_header(mbuf);
        struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(iphdr + 1);

        LOG_INFO("tcp rx " IP_FMT ":%u -> " IP_FMT ":%u flags=%s seq=%u ack=%u",
                 IP_ARG(iphdr->src_addr), rte_be_to_cpu_16(tcp_hdr->src_port),
                 IP_ARG(iphdr->dst_addr), rte_be_to_cpu_16(tcp_hdr->dst_port),
                 tcp_flags_str(tcp_hdr->tcp_flags), ntohl(tcp_hdr->sent_seq),
                 ntohl(tcp_hdr->recv_ack));

        /* Learn client MAC from the frame so SYN+ACK need not wait on ARP. */
        arp_table_add(iphdr->src_addr, eth->src_addr.addr_bytes);

        struct tcp_stream *stream = tcp_stream_search(
            iphdr->src_addr, iphdr->dst_addr, tcp_hdr->src_port,
            tcp_hdr->dst_port, IPPROTO_TCP);

        if (stream == NULL) {
                stream = tcp_stream_create(iphdr->src_addr, iphdr->dst_addr,
                                           tcp_hdr->src_port, tcp_hdr->dst_port,
                                           IPPROTO_TCP);
                if (stream == NULL) {
                        rte_exit(EXIT_FAILURE, "tcp_stream_create failed\n");
                }
        } else {
                LOG_DEBUG(
                    "tcp stream hit status=%s " IP_FMT ":%u <-> " IP_FMT ":%u",
                    tcp_status_str(stream->status), IP_ARG(stream->src_ip),
                    rte_be_to_cpu_16(stream->src_port), IP_ARG(stream->dst_ip),
                    rte_be_to_cpu_16(stream->dst_port));
        }

        switch (stream->status) {
        case TCP_STATUS_CLOSED:
                // TODO:
                break;
        case TCP_STATUS_LISTEN:
                tcp_stream_handle_listen(stream, tcp_hdr);
                break;
        case TCP_STATUS_SYN_RECV:
                tcp_stream_handle_syn_recv(stream, tcp_hdr);
                break;
        case TCP_STATUS_SYN_SENT:
                // TODO:
                break;
        case TCP_STATUS_ESTABLISHED: {
                // TODO: Improve processing procedures
                // just copy the data from the send buffer to the receive buffer
                uint8_t hdrlen = tcp_hdr->data_off * 4;
                uint8_t *payload = (uint8_t *)(tcp_hdr + hdrlen);

                LOG_INFO("Received data: %s", payload);
                break;
        }
        case TCP_STATUS_CLOSE_WAIT:
                // TODO:
                break;
        case TCP_STATUS_LAST_ACK:
                // TODO:
                break;
        case TCP_STATUS_TIME_WAIT:
                // TODO:
                break;
        case TCP_STATUS_FIN_WAIT_1:
                // TODO:
                break;
        case TCP_STATUS_FIN_WAIT_2:
                // TODO:
                break;
        default:
                // TODO:
                break;
        }
        return 0;
}

static int encode_tcp_pkt(uint8_t *msg, uint32_t src_ip, uint32_t dst_ip,
                          uint8_t *src_mac, uint8_t *dst_mac,
                          struct tcp_fragment *fragment) {

        const unsigned total_len =
            fragment->payload_len + sizeof(struct rte_ether_hdr) +
            sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr) +
            fragment->opt_len * sizeof(uint32_t);

        // 1 ethhdr
        struct rte_ether_hdr *eth = (struct rte_ether_hdr *)msg;
        rte_memcpy(eth->src_addr.addr_bytes, src_mac, RTE_ETHER_ADDR_LEN);
        rte_memcpy(eth->dst_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
        eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);

        // 2 iphdr
        struct rte_ipv4_hdr *ip =
            (struct rte_ipv4_hdr *)(msg + sizeof(struct rte_ether_hdr));
        ip->version_ihl = 0x45;
        ip->type_of_service = 0;
        ip->total_length = htons(total_len - sizeof(struct rte_ether_hdr));
        ip->packet_id = 0;
        ip->fragment_offset = 0;
        ip->time_to_live = 64; // ttl = 64
        ip->next_proto_id = IPPROTO_TCP;
        ip->src_addr = src_ip;
        ip->dst_addr = dst_ip;

        ip->hdr_checksum = 0;
        ip->hdr_checksum = rte_ipv4_cksum(ip);

        // 3 tcphdr
        struct rte_tcp_hdr *tcp =
            (struct rte_tcp_hdr *)(msg + sizeof(struct rte_ether_hdr) +
                                   sizeof(struct rte_ipv4_hdr));
        tcp->src_port = fragment->src_port;
        tcp->dst_port = fragment->dst_port;
        tcp->sent_seq = htonl(fragment->sent_seq);
        tcp->recv_ack = htonl(fragment->recv_ack);

        tcp->data_off = fragment->data_off;
        tcp->tcp_flags = fragment->tcp_flags;
        tcp->rx_win = htons(fragment->rx_win);
        tcp->tcp_urp = fragment->tcp_urp;
        tcp->cksum = 0;
        tcp->cksum = rte_ipv4_udptcp_cksum(ip, tcp);

        // 4 payload
        if (fragment->payload_len > 0) {
                rte_memcpy(msg + sizeof(struct rte_ether_hdr) +
                               sizeof(struct rte_ipv4_hdr) +
                               sizeof(struct rte_tcp_hdr),
                           fragment->payload, fragment->payload_len);
        }
        return 0;
}

static struct rte_mbuf *tcp_build_pkt(struct rte_mempool *mp, uint32_t src_ip,
                                      uint32_t dst_ip, uint8_t *src_mac,
                                      uint8_t *dst_mac,
                                      struct tcp_fragment *fragment) {
        const unsigned total_len =
            fragment->payload_len + sizeof(struct rte_ether_hdr) +
            sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr) +
            fragment->opt_len * sizeof(uint32_t);

        struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mp);
        if (!mbuf) {
                LOG_ERROR("rte_pktmbuf_alloc failed\n");
                return NULL;
        }

        mbuf->pkt_len = total_len;
        mbuf->data_len = total_len;

        uint8_t *pkt_data = rte_pktmbuf_mtod(mbuf, uint8_t *);
        encode_tcp_pkt(pkt_data, src_ip, dst_ip, src_mac, dst_mac, fragment);
        return mbuf;
}
void tcp_out(struct rte_mempool *mp) {
        struct tcp_table *table = tcp_table_instance();
        struct tcp_stream *stream = NULL;
        for (stream = table->tcb_set; stream != NULL; stream = stream->next) {
                /* SYN_RECV: send SYN+ACK during handshake; ESTABLISHED: data */
                if (stream->status != TCP_STATUS_SYN_RECV &&
                    stream->status != TCP_STATUS_ESTABLISHED)
                        continue;

                struct tcp_fragment *fragment = NULL;
                int nb_sent =
                    rte_ring_sc_dequeue(stream->send_buf, (void **)&fragment);
                if (nb_sent < 0)
                        continue;

                /* stream->src_* is the remote peer (client); reply goes there
                 */
                uint32_t peer_ip = stream->src_ip;
                uint8_t *dst_mac = arp_lookup(peer_ip);
                if (dst_mac == NULL) {
                        LOG_INFO("tcp tx wait ARP for " IP_FMT
                                 " flags=%s seq=%u ack=%u",
                                 IP_ARG(peer_ip),
                                 tcp_flags_str(fragment->tcp_flags),
                                 fragment->sent_seq, fragment->recv_ack);

                        struct rte_mbuf *arp = arp_build_pkt(
                            mp, RTE_ARP_OP_REQUEST, g_broadcast_mac,
                            g_net.local_ip, peer_ip);
                        if (arp == NULL) {
                                rte_exit(EXIT_FAILURE,
                                         "arp_build_pkt failed\n");
                        }
                        struct inout_ring *ring = ring_instance();
                        rte_ring_mp_enqueue_burst(ring->out, (void **)&arp, 1,
                                                  NULL);

                        rte_ring_mp_enqueue(stream->send_buf, fragment);
                } else {
                        struct rte_mbuf *tcp_buf =
                            tcp_build_pkt(mp, g_net.local_ip, peer_ip,
                                          g_net.local_mac, dst_mac, fragment);
                        if (tcp_buf == NULL) {
                                rte_free(fragment);
                                continue;
                        }

                        struct inout_ring *ring = ring_instance();
                        rte_ring_mp_enqueue_burst(ring->out, (void **)&tcp_buf,
                                                  1, NULL);

                        if ((fragment->tcp_flags &
                             (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG)) ==
                            (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG)) {
                                LOG_INFO(
                                    "tcp handshake [2/3] SYN+ACK tx " IP_FMT
                                    ":%u -> " IP_FMT ":%u seq=%u ack=%u",
                                    IP_ARG(g_net.local_ip),
                                    rte_be_to_cpu_16(fragment->src_port),
                                    IP_ARG(peer_ip),
                                    rte_be_to_cpu_16(fragment->dst_port),
                                    fragment->sent_seq, fragment->recv_ack);
                        } else {
                                LOG_INFO("tcp tx " IP_FMT ":%u -> " IP_FMT
                                         ":%u flags=%s seq=%u ack=%u len=%zu",
                                         IP_ARG(g_net.local_ip),
                                         rte_be_to_cpu_16(fragment->src_port),
                                         IP_ARG(peer_ip),
                                         rte_be_to_cpu_16(fragment->dst_port),
                                         tcp_flags_str(fragment->tcp_flags),
                                         fragment->sent_seq, fragment->recv_ack,
                                         fragment->payload_len);
                        }

                        rte_free(fragment);
                }
        }
}