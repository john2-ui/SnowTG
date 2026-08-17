/**
 * @file tcp.c
 * @brief TCP control block, table-driven state machine, encode/egress, and the
 *        tcp_ops vector consumed by the unified socket layer.
 *
 * Inbound:  tcp_ingress -> state handler -> owner-local RX/control queues
 * Outbound: tcp_tx_flush -> ARP resolve -> worker output ring -> NIC
 */
#include "tcp.h"
#include "arp.h"
#include "config.h"
#include "log.h"
#include "net_context.h"
#include "owner_io.h"
#include "pkt_frame.h"
#include "port.h"
#include "rbtree.h"
#include "ring.h"
#include "rx_dispatch.h"
#include "socket.h"
#include "socket_owner_internal.h"
#include "stack_runtime.h"
#include "tcp_memory.h"
#include "tcp_ofo.h"
#include "tcp_options.h"
#include "tcp_rtt.h"

#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <rte_bitops.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_ip4.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>
#include <rte_random.h>
#include <rte_ring.h>
#include <rte_tcp.h>
#include <rte_timer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

static inline void tcp_mark_tx_dirty(struct nsock *sk) {
#ifndef TCP_TESTING
        nsock_tx_mark_dirty(sk);
#else
        (void)sk;
#endif
}

/*
 * Stable connection identity for lifecycle and diagnostic logs.  Application
 * fds live in a separate handle table and can be reused, so sock/generation
 * identify the TCB.
 */
#define TCP_ID_FMT "sock=%u gen=%u"
#define TCP_ID_ARG(sk) (sk)->id, (sk)->generation
#define TCP_SK_FMT TCP_ID_FMT " state=%s local=" IP_FMT ":%u peer=" IP_FMT ":%u"
#define TCP_SK_ARG(sk)                                                         \
        TCP_ID_ARG(sk), tcp_status_str((sk)->u.tcp.status),                    \
            IP_ARG((sk)->local_ip), rte_be_to_cpu_16((sk)->local_port),        \
            IP_ARG((sk)->u.tcp.remote_ip),                                     \
            rte_be_to_cpu_16((sk)->u.tcp.remote_port)

static void tcp_drain_send(struct nsock *sk);
static void tcp_drain_recv(struct nsock *sk);

/*
 * Production TCP allocation occurs only on a socket owner.  TCP_TESTING also
 * exercises reassembly helpers without creating an owner, so the small
 * fallback preserves that isolated seam without changing the runtime path.
 */
#ifdef TCP_TESTING
/** Test seam: disable owner-pool access for isolated TCP algorithm tests. */
static inline struct tcp_owner_memory *tcp_memory_current(void) { return NULL; }

/** Test seam: release fallback heap payload storage without a TCP owner. */
static inline void tcp_payload_release(uint8_t *data,
                                       __attribute__((unused)) void *storage) {
        rte_free(data);
}
#else
/** Return the current lcore's TCP pool domain, if it is the socket owner. */
static struct tcp_owner_memory *tcp_memory_current(void) {
        return socket_owner_tcp_memory();
}

/** Return pool-backed payload storage, or fallback storage in a test seam. */
static void tcp_payload_release(uint8_t *data, void *storage) {
        struct tcp_owner_memory *memory = tcp_memory_current();

        if (storage != NULL && memory != NULL)
                tcp_memory_payload_free(memory, storage);
        else if (data != NULL)
                rte_free(data);
}
#endif

static struct rte_mbuf *tcp_build_pkt(struct rte_mempool *mp, uint32_t src_ip,
                                      uint32_t dst_ip, const uint8_t *dst_mac,
                                      const struct tcp_fragment *f);
static void tcp_emit_fragment(uint32_t src_ip, uint32_t dst_ip,
                              const uint8_t *dst_mac,
                              const struct tcp_fragment *f);
static void tcp_send_reset_reply(const struct rte_ether_hdr *eth,
                                 const struct rte_ipv4_hdr *iphdr,
                                 const struct rte_tcp_hdr *tcp_hdr);
static void tcp_send_reset_for_stream(const struct nsock *sk,
                                      const uint8_t *dst_mac);
static int tcp_rst_acceptable(const struct nsock *sk,
                              const struct rte_tcp_hdr *hdr);
static void tcp_abort_on_rst(struct nsock *sk);

/**
 * @brief Choose the smallest Window Scale that represents @p rcvbuf_size.
 * @return A valid RFC 7323 scale in the inclusive range [0, 14].
 */
static uint8_t tcp_choose_rcv_wscale(uint32_t rcvbuf_size) {
        uint8_t scale = 0;

        while (scale < TCP_WSCALE_MAX && (rcvbuf_size >> scale) > UINT16_MAX)
                scale++;

        return scale;
}

/**
 * @brief Return the currently advertisable unscaled receive window.
 * @param sk TCP stream whose receive-buffer accounting is queried.
 * @return Available receive-buffer space before TCP-header encoding.
 *
 * Both packet ingress and application RECV commands execute on the owner
 * worker, which exclusively owns @c rcvbuf_used.
 */
static uint32_t tcp_rcv_wnd(const struct nsock *sk) {
        if (sk->u.tcp.rcvbuf_used >= sk->u.tcp.rcvbuf_size)
                return 0;

        return sk->u.tcp.rcvbuf_size - sk->u.tcp.rcvbuf_used;
}

/**
 * @brief Encode the current receive window for an outbound TCP header.
 *
 * SYN and SYN+ACK windows are never scaled.  After a successful Window Scale
 * negotiation, all other advertised windows use this endpoint's scale.
 */
static uint16_t tcp_wire_rcv_wnd(const struct nsock *sk, uint8_t flags) {
        uint32_t wnd = tcp_rcv_wnd(sk);
        uint8_t scale = 0;

        /*
         * SYN and SYN+ACK carry an unscaled window. The negotiated scale
         * applies only to subsequent segments.
         */
        if (!(flags & RTE_TCP_SYN_FLAG) && sk->u.tcp.wscale_ok)
                scale = sk->u.tcp.rcv_wscale;

        wnd >>= scale;
        return (uint16_t)(wnd > UINT16_MAX ? UINT16_MAX : wnd);
}

/** @brief Test whether @p a precedes @p b in the TCP serial number space.
 * @param a First sequence number.
 * @param b Second sequence number.
 * @return Non-zero when @p a is before @p b modulo 2^32.
 */
static inline int tcp_seq_lt(uint32_t a, uint32_t b) {
        return (int32_t)(a - b) < 0;
}
/** @brief Test whether @p a is not after @p b in the TCP serial number space.
 * @param a First sequence number.
 * @param b Second sequence number.
 * @return Non-zero when @p a is before or equal to @p b modulo 2^32.
 */
static inline int tcp_seq_leq(uint32_t a, uint32_t b) {
        return (int32_t)(a - b) <= 0;
}
/** @brief Test whether @p a follows @p b in the TCP serial number space.
 * @param a First sequence number.
 * @param b Second sequence number.
 * @return Non-zero when @p a is after @p b modulo 2^32.
 */
static inline int tcp_seq_gt(uint32_t a, uint32_t b) {
        return (int32_t)(a - b) > 0;
}

/**
 * @brief Test whether an inbound payload/FIN range intersects RCV.WND.
 *
 * This is the RFC 793 receive-window test shared by Timestamp PAWS and the
 * ESTABLISHED data path.  The zero-window special case accepts only an empty
 * probe at RCV.NXT; with a nonzero window, an empty ACK is accepted when its
 * sequence number lies in the window.
 */
static int tcp_segment_acceptable(const struct nsock *sk, uint32_t seq,
                                  uint16_t payload_len, bool has_fin) {
        uint32_t rcv_nxt = sk->u.tcp.recv_ack;
        uint32_t rcv_wnd = tcp_rcv_wnd(sk);
        uint32_t seg_len = payload_len + (has_fin ? 1 : 0);
        uint32_t seg_end = seq + seg_len;

        if (rcv_wnd == 0)
                return seg_len == 0 && seq == rcv_nxt;

        if (seg_len == 0)
                return !tcp_seq_lt(seq, rcv_nxt) &&
                       tcp_seq_lt(seq, rcv_nxt + rcv_wnd);

        return tcp_seq_lt(seq, rcv_nxt + rcv_wnd) &&
               tcp_seq_gt(seg_end, rcv_nxt);
}

/**
 * @brief Record the first duplicate payload subrange in the current segment.
 * @param sk Stream whose next ACK may carry the resulting D-SACK.
 * @param seq First payload sequence number in the received segment.
 * @param len Received payload length.
 *
 * Bytes below RCV.NXT are already cumulatively acknowledged.  Bytes above it
 * are duplicates only when they overlap data actually retained in OFO.  The
 * first determinable duplicate interval becomes a one-shot RFC 2883 block.
 */
static void tcp_sack_note_duplicate(struct nsock *sk, uint32_t seq,
                                    uint32_t len) {
        uint32_t end = seq + len;

        sk->u.tcp.dsack_pending = false;
        if (!sk->u.tcp.sack_permitted || len == 0)
                return;
        if (tcp_seq_lt(seq, sk->u.tcp.recv_ack)) {
                uint32_t right = tcp_seq_lt(end, sk->u.tcp.recv_ack)
                                     ? end
                                     : sk->u.tcp.recv_ack;
                if (tcp_seq_lt(seq, right)) {
                        sk->u.tcp.dsack_block.left = seq;
                        sk->u.tcp.dsack_block.right = right;
                        sk->u.tcp.dsack_pending = true;
                        return;
                }
        }
        for (const struct tcp_ofo_seg *seg = sk->u.tcp.ofo; seg != NULL;
             seg = seg->next) {
                uint32_t seg_end = seg->seq + seg->len;
                uint32_t left;
                uint32_t right;

                if (!tcp_seq_lt(seg->seq, end))
                        break;
                if (!tcp_seq_lt(seq, seg_end))
                        continue;
                left = tcp_seq_gt(seq, seg->seq) ? seq : seg->seq;
                right = tcp_seq_lt(end, seg_end) ? end : seg_end;
                if (tcp_seq_lt(left, right)) {
                        sk->u.tcp.dsack_block.left = left;
                        sk->u.tcp.dsack_block.right = right;
                        sk->u.tcp.dsack_pending = true;
                        return;
                }
        }
}

/**
 * @brief Resolve a received trigger against bytes actually retained in OFO.
 * @param sk Stream whose next SACK ACK should prioritize this trigger.
 * @param seq First payload sequence number offered for OFO insertion.
 * @param len Offered payload length.
 *
 * Recomputing after insertion prevents capacity-rejected bytes from being
 * advertised and limits partial-insert reports to data that really exists.
 */
static void tcp_sack_note_recent(struct nsock *sk, uint32_t seq,
                                 uint32_t len) {
        uint32_t trigger_end = seq + len;

        sk->u.tcp.sack_recent_valid = false;
        if (len == 0)
                return;

        for (const struct tcp_ofo_seg *seg = sk->u.tcp.ofo; seg != NULL;
             seg = seg->next) {
                uint32_t seg_end = seg->seq + seg->len;

                if (seg->len == 0 || !tcp_seq_gt(seg_end, seq) ||
                    !tcp_seq_gt(trigger_end, seg->seq))
                        continue;
                sk->u.tcp.sack_recent.left =
                    tcp_seq_gt(seg->seq, seq) ? seg->seq : seq;
                sk->u.tcp.sack_recent.right =
                    tcp_seq_lt(seg_end, trigger_end) ? seg_end : trigger_end;
                sk->u.tcp.sack_recent_valid = true;
        }
}

/** @brief Copy contiguous TCP payload into the application receive queue.
 * @param sk Destination TCP socket.
 * @param data Payload bytes to copy.
 * @param len Number of payload bytes.
 * @return 0 on successful queueing, or -1 on allocation or queue failure.
 */
static int tcp_deliver_payload(struct nsock *sk, const uint8_t *data,
                               uint32_t len) {
        struct tcp_owner_memory *memory;
        void *storage = NULL;
        if (len == 0)
                return 0;
        if (len > TCP_MEMORY_CHUNK_SIZE)
                return -1;
        memory = tcp_memory_current();
        struct tcp_rx_blob *b = memory == NULL
                                    ? rte_zmalloc("tcp_rx_blob", sizeof(*b), 0)
                                    : tcp_memory_rx_blob_alloc(memory);
        if (b == NULL)
                return -1;
        if ((memory != NULL &&
             tcp_memory_payload_alloc(memory, &b->data, &storage) != 0) ||
            (memory == NULL &&
             (b->data = rte_malloc("tcp_rx_data", len, 0)) == NULL)) {
                if (memory != NULL)
                        tcp_memory_rx_blob_free(memory, b);
                else
                        rte_free(b);
                return -1;
        }
        rte_memcpy(b->data, data, len);
        b->len = len;
        b->off = 0;
        b->storage = storage;

        if (nsock_tcp_rx_enqueue(sk, b) != 0) {
                LOG_ERROR("tcp recv_buf full " TCP_ID_FMT ", dropping %u bytes",
                          TCP_ID_ARG(sk), len);
                tcp_payload_release(b->data, b->storage);
                if (memory != NULL)
                        tcp_memory_rx_blob_free(memory, b);
                else
                        rte_free(b);
                return -1;
        }
        /*
         * The caller commits recv_ack and rcvbuf_used before waking a parked
         * receive command.  Waking here would re-enter tcp_recv() while those
         * TCP receive-side state variables still describe the prior segment.
         */
        return 0;
}


/**
 * @brief Detect duplicates, insert OFO payload, and derive the ACK trigger.
 * @return Result from @ref tcp_ofo_queue_insert.
 */
static int tcp_ofo_insert(struct nsock *sk, uint32_t seq, const uint8_t *data,
                          uint32_t len, int has_fin) {
        uint32_t trigger_seq = seq;
        uint32_t trigger_len = len;
        tcp_sack_note_duplicate(sk, seq, len);
        int result = tcp_ofo_queue_insert(sk, seq, data, len, has_fin);

        tcp_sack_note_recent(sk, trigger_seq, trigger_len);
        return result;
}

static void tcp_ofo_enter_close_wait(struct nsock *sk) {
        tcp_stream_set_status(sk, TCP_STATUS_CLOSE_WAIT);
}

#ifdef TCP_TESTING
int tcp_test_ofo_insert(struct nsock *sk, uint32_t seq, const uint8_t *data,
                        uint32_t len, int has_fin) {
        return tcp_ofo_insert(sk, seq, data, len, has_fin);
}

void tcp_test_ofo_drain(struct nsock *sk) {
        tcp_ofo_drain(sk, tcp_deliver_payload, tcp_ofo_enter_close_wait);
}
#endif


/** @brief Initialize an empty, lazily allocated TCP send buffer.
 * @param sb Send buffer to initialize.
 * @param isn Initial sequence number for the buffer head.
 * @return 0 on success, or -1 for an invalid destination.
 */
int tcp_sndbuf_init(struct tcp_sndbuf *sb, uint32_t isn) {
        if (sb == NULL)
                return -1;
        sb->head = NULL;
        sb->tail = NULL;
        sb->len = 0;
        sb->head_seq = isn;
        return 0;
}

/** @brief Release storage owned by a TCP send buffer.
 * @param sb Send buffer to release.
 */
void tcp_sndbuf_free(struct tcp_sndbuf *sb) {
        struct tcp_owner_memory *memory;

        if (sb == NULL)
                return;
        memory = tcp_memory_current();
        while (sb->head != NULL) {
                struct tcp_tx_chunk *chunk = sb->head;

                sb->head = chunk->next;
                tcp_payload_release(chunk->data, chunk->storage);
                if (memory != NULL)
                        tcp_memory_tx_chunk_free(memory, chunk);
                else
                        rte_free(chunk);
        }
        sb->tail = NULL;
        sb->len = 0;
}

/** @brief Empty a send buffer and assign its new sequence base.
 * @param sb Send buffer to reset.
 * @param seq Sequence number for the new buffer head.
 */
static void tcp_sndbuf_reset(struct tcp_sndbuf *sb, uint32_t seq) {
        tcp_sndbuf_free(sb);
        sb->head_seq = seq;
}

/** @brief Append application bytes to the TCP send buffer.
 *
 * The caller must run on the owning lcore. This helper only enforces physical
 * buffer capacity; application admission against the local high-water mark
 * and peer window is handled by @ref tcp_send.
 *
 * @param sb Destination send buffer.
 * @param data Source bytes.
 * @param len Requested byte count.
 * @return Accepted byte count (possibly short), or -1 if full or invalid.
 */
static ssize_t tcp_sndbuf_append(struct tcp_sndbuf *sb, const uint8_t *data,
                                 size_t len) {
        struct tcp_owner_memory *memory;
        size_t copied = 0;

        if (sb == NULL || data == NULL || len == 0)
                return 0;
        memory = tcp_memory_current();

        while (copied < len) {
                struct tcp_tx_chunk *chunk = sb->tail;
                size_t space = 0;

                if (chunk != NULL)
                        space = TCP_MEMORY_CHUNK_SIZE - chunk->len;
                if (space == 0) {
                        void *storage = NULL;

                        chunk =
                            memory == NULL
                                ? rte_zmalloc("tcp_tx_chunk", sizeof(*chunk), 0)
                                : tcp_memory_tx_chunk_alloc(memory);
                        if (chunk == NULL)
                                break;
                        if ((memory != NULL &&
                             tcp_memory_payload_alloc(memory, &chunk->data,
                                                      &storage) != 0) ||
                            (memory == NULL &&
                             (chunk->data = rte_malloc("tcp_tx_data",
                                                       TCP_MEMORY_CHUNK_SIZE,
                                                       0)) == NULL)) {
                                if (memory != NULL)
                                        tcp_memory_tx_chunk_free(memory, chunk);
                                else
                                        rte_free(chunk);
                                break;
                        }
                        chunk->storage = storage;
                        chunk->seq = sb->head_seq + sb->len;
                        if (sb->tail != NULL)
                                sb->tail->next = chunk;
                        else
                                sb->head = chunk;
                        sb->tail = chunk;
                        space = TCP_MEMORY_CHUNK_SIZE;
                }
                size_t take = len - copied < space ? len - copied : space;

                rte_memcpy(chunk->data + chunk->len, data + copied, take);
                chunk->len += (uint16_t)take;
                sb->len += (uint32_t)take;
                copied += take;
        }
        return copied == 0 ? -1 : (ssize_t)copied;
}

/**
 * @brief Return the application-admission limit for TCP send buffering.
 *
 * Before the handshake supplies a peer window, the local high-water mark is
 * used.  Afterwards, the accepted application data must not exceed the smaller
 * of that mark and the peer's most recently advertised receive window.
 *
 * This helper runs on the socket's owner lcore.
 * @param sk TCP socket whose send capacity is queried.
 * @return Maximum number of bytes that may remain in @c sndbuf.
 */
static uint32_t tcp_app_snd_limit(const struct nsock *sk) {
        uint32_t limit = TCP_SNDBUF_APP_HIWAT;

        if (sk->u.tcp.snd_wnd_valid && sk->u.tcp.snd_wnd < limit)
                limit = sk->u.tcp.snd_wnd;

        return limit;
}

/**
 * @brief Return immediately writable application-buffer space.
 *
 * This helper runs on the socket's owner lcore.
 * @param sk TCP socket whose available send-buffer space is queried.
 * @return Bytes admissible to @ref tcp_send, or zero when it must wait.
 */
static uint32_t tcp_app_snd_space(const struct nsock *sk) {
        uint32_t limit = tcp_app_snd_limit(sk);

        if (sk->u.tcp.sndbuf.len >= limit)
                return 0;

        return limit - sk->u.tcp.sndbuf.len;
}

/** @brief Remove acknowledged bytes from the head of a send buffer.
 * @param sb Send buffer to advance.
 * @param len Requested number of bytes to remove; clamped to buffered bytes.
 */
static void tcp_sndbuf_remove(struct tcp_sndbuf *sb, uint32_t len) {
        struct tcp_owner_memory *memory;

        if (sb == NULL)
                return;
        memory = tcp_memory_current();
        if (len == 0)
                return;
        if (len > sb->len)
                len = sb->len;
        while (len != 0 && sb->head != NULL) {
                struct tcp_tx_chunk *chunk = sb->head;
                uint32_t available = chunk->len - chunk->off;
                uint32_t take = len < available ? len : available;

                chunk->off += (uint16_t)take;
                sb->len -= take;
                sb->head_seq += take;
                len -= take;
                if (chunk->off == chunk->len) {
                        sb->head = chunk->next;
                        if (sb->head == NULL)
                                sb->tail = NULL;
                        tcp_payload_release(chunk->data, chunk->storage);
                        if (memory != NULL)
                                tcp_memory_tx_chunk_free(memory, chunk);
                        else
                                rte_free(chunk);
                }
        }
}

/** Return the payload range beginning at @p seq, capped to one chunk. */
static const uint8_t *tcp_sndbuf_peek(const struct tcp_sndbuf *sb, uint32_t seq,
                                      uint32_t *available) {
        uint32_t skip;

        if (sb == NULL || available == NULL || tcp_seq_lt(seq, sb->head_seq) ||
            !tcp_seq_lt(seq, sb->head_seq + sb->len))
                return NULL;
        skip = seq - sb->head_seq;
        for (const struct tcp_tx_chunk *chunk = sb->head; chunk != NULL;
             chunk = chunk->next) {
                uint32_t bytes = chunk->len - chunk->off;

                if (skip < bytes) {
                        *available = bytes - skip;
                        return chunk->data + chunk->off + skip;
                }
                skip -= bytes;
        }
        return NULL;
}

#ifdef TCP_TESTING
/**
 * Initialize a test send buffer and append bytes through the production chunk
 * allocator; isolated tests use the fallback heap path.
 */
int tcp_test_sndbuf_append(struct nsock *sk, uint32_t isn, const uint8_t *data,
                           size_t len) {
        if (sk == NULL)
                return -1;
        if (tcp_sndbuf_init(&sk->u.tcp.sndbuf, isn) != 0)
                return -1;
        return (int)tcp_sndbuf_append(&sk->u.tcp.sndbuf, data, len);
}

/** Remove ACKed test bytes, including any complete chunk reclamation. */
void tcp_test_sndbuf_remove(struct nsock *sk, uint32_t len) {
        if (sk != NULL)
                tcp_sndbuf_remove(&sk->u.tcp.sndbuf, len);
}

/** Expose one contiguous test payload range at a TCP sequence number. */
const uint8_t *tcp_test_sndbuf_peek(const struct nsock *sk, uint32_t seq,
                                    uint32_t *available) {
        return sk == NULL ? NULL
                          : tcp_sndbuf_peek(&sk->u.tcp.sndbuf, seq, available);
}

/** Release all test send-buffer chunks. */
void tcp_test_sndbuf_free(struct nsock *sk) {
        if (sk != NULL)
                tcp_sndbuf_free(&sk->u.tcp.sndbuf);
}
#endif

/** @brief Return a printable TCP state name.
 * @param s TCP state to format.
 * @return Static state-name string.
 */
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

/** @brief Format TCP flags for logging.
 * @param flags TCP header flag bitmap.
 * @return Pointer to a static buffer overwritten by the next call.
 */
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

/** @brief Find a TCP listener bound to an exact local endpoint.
 * @param local_ip Local IPv4 address in network byte order.
 * @param local_port Local TCP port in network byte order.
 * @return Matching LISTEN socket, or NULL if none exists.
 */
static struct nsock *tcp_listener_lookup(uint32_t local_ip,
                                         uint16_t local_port) {
        return nsock_from_ip_port(local_ip, local_port, IPPROTO_TCP);
}

/** @brief Look up a TCP stream by its complete four-tuple.
 * @param remote_ip Peer IPv4 address in network byte order.
 * @param local_ip Local IPv4 address in network byte order.
 * @param remote_port Peer TCP port in network byte order.
 * @param local_port Local TCP port in network byte order.
 * @return Matching TCP socket, or NULL if absent.
 */
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
/** @brief Produce the next process-local initial sequence number.
 * @return Newly generated ISN in host byte order.
 */
static uint32_t tcp_next_isn(void) {
        if (!tcp_isn_inited) {
                tcp_isn_state = (uint32_t)time(NULL);
                tcp_isn_inited = 1;
        }
        tcp_isn_state += 0x9e3779b1u; /* golden-ratio-ish step per stream */
        return tcp_isn_state;
}

/**
 * @brief Create a passive-open child TCP control block without an fd.
 *
 * No fd yet: the peer can flood SYNs and we
 * still need TCBs + rings, but userspace fds are only handed out in tcp_accept
 * after the 3WHS completes.
 * @param remote_ip Peer IPv4 address in network byte order.
 * @param local_ip Local IPv4 address in network byte order.
 * @param remote_port Peer TCP port in network byte order.
 * @param local_port Local TCP port in network byte order.
 * @return Newly allocated SYN_RECV child, or NULL on allocation failure.
 */
struct nsock *tcp_stream_create(uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port) {
        struct nsock *sk = nsock_alloc(IPPROTO_TCP);
        if (sk == NULL) {
                LOG_ERROR("tcp_stream_create: nsock_alloc failed");
                return NULL;
        }

        /*
         * Passive children have no application fd yet, but they still need an
         * owner slot immediately: timers, listener queues, and the four-tuple
         * index may retain them before ACCEPT publishes a handle.
         */
        if (socket_owner_adopt(sk) != 0) {
                LOG_ERROR("tcp_stream_create: owner slot table full");
                nsock_free(sk);
                return NULL;
        }

        sk->local_ip = local_ip;
        sk->local_port = local_port;
        sk->u.tcp.remote_ip = remote_ip;
        sk->u.tcp.remote_port = remote_port;
        sk->u.tcp.status = TCP_STATUS_SYN_RECV;
        if (nsock_tcp_conn_register(sk) != 0) {
                nsock_free(sk);
                return NULL;
        }
        sk->u.tcp.listener = NULL;
        sk->u.tcp.sent_seq = tcp_next_isn();
        sk->u.tcp.snd_una = sk->u.tcp.sent_seq;
        /* sndbuf already allocated in nsock_alloc; only reset sequence base. */
        tcp_sndbuf_reset(&sk->u.tcp.sndbuf, sk->u.tcp.sent_seq);
        sk->u.tcp.recv_ack = 0;
        sk->u.tcp.rcv_wscale = tcp_choose_rcv_wscale(sk->u.tcp.rcvbuf_size);
        sk->u.tcp.snd_wscale = 0;
        sk->u.tcp.wscale_ok = false;

        tcp_options_reset_state(sk);
        tcp_rtt_reset(sk);
        sk->u.tcp.syn_retransmitted = false;
        tcp_cc_init_default(&sk->u.tcp, false);

        LOG_TCP_INFO(TCP_SK_FMT " event=stream-create isn=%u "
                                "fd_deferred=1",
                     TCP_SK_ARG(sk), sk->u.tcp.sent_seq);
        return sk;
}

/** @brief Transition a stream to a new TCP state and log the change.
 * @param sk Stream whose state changes.
 * @param new_status Destination TCP state.
 */
void tcp_stream_set_status(struct nsock *sk, TCP_STATUS new_status) {
        TCP_STATUS old = sk->u.tcp.status;
        if (old != TCP_STATUS_CLOSED && new_status == TCP_STATUS_CLOSED) {
                nsock_tcp_conn_unregister(sk);
        }
        sk->u.tcp.status = new_status;
        LOG_TCP_INFO(TCP_SK_FMT " event=state-transition from=%s to=%s",
                     TCP_SK_ARG(sk), tcp_status_str(old),
                     tcp_status_str(new_status));
}

/** @brief Locate the IPv4 header in an Ethernet mbuf.
 * @param mbuf Packet containing Ethernet followed by IPv4.
 * @return Pointer to the packet's IPv4 header.
 */
static struct rte_ipv4_hdr *tcp_ipv4_header(struct rte_mbuf *mbuf) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        return (struct rte_ipv4_hdr *)(eth + 1);
}

/**
 * @brief Allocate a zero-initialized outbound TCP fragment descriptor.
 * @return Allocated descriptor, or NULL when the owner pool is exhausted.
 */
static struct tcp_fragment *tcp_fragment_alloc(void) {
        struct tcp_owner_memory *memory = tcp_memory_current();
        struct tcp_fragment *f;

        f = memory == NULL ? rte_zmalloc("tcp_fragment", sizeof(*f), 0)
                           : tcp_memory_fragment_alloc(memory);
        return f;
}

/** Release a control fragment and its optional independently-owned payload. */
static void tcp_fragment_free(struct tcp_fragment *f) {
        struct tcp_owner_memory *memory;

        if (f == NULL)
                return;
        if (f->payload != NULL)
                rte_free(f->payload);
        memory = tcp_memory_current();
        if (memory != NULL)
                tcp_memory_fragment_free(memory, f);
        else
                rte_free(f);
}

/** @brief Queue an outbound TCP fragment and release it on failure.
 * @param sk Socket owning the outbound queue.
 * @param f Fragment to enqueue; consumed in both success and failure paths.
 * @return 0 on success, or -1 if send_buf is full.
 */
static int tcp_enqueue_fragment(struct nsock *sk, struct tcp_fragment *f) {
        if (f == NULL)
                return -1;
        if (nsock_tcp_tx_enqueue(sk, f) != 0) {
                LOG_ERROR("tcp send_buf full for " TCP_ID_FMT " flags=0x%02x",
                          TCP_ID_ARG(sk), f->tcp_flags);
                tcp_fragment_free(f);
                return -1;
        }
        tcp_mark_tx_dirty(sk);
        return 0;
}

/**
 * @brief Build a header-only or payload-bearing outbound fragment descriptor.
 *
 * Ports and sequence numbers follow tcp_fragment conventions: ports network
 * order, sent_seq/recv_ack/rx_win host order.
 * @param sk Stream supplying local and peer ports.
 * @param flags TCP flags for the fragment.
 * @param sent_seq Segment sequence number in host byte order.
 * @param recv_ack ACK number in host byte order.
 * @return Newly allocated fragment descriptor.
 */
static struct tcp_fragment *tcp_make_fragment(struct nsock *sk, uint8_t flags,
                                              uint32_t sent_seq,
                                              uint32_t recv_ack) {
        struct tcp_fragment *f = tcp_fragment_alloc();
        if (f == NULL)
                return NULL;
        f->src_port = sk->local_port;
        f->dst_port = sk->u.tcp.remote_port;
        f->sent_seq = sent_seq;
        f->recv_ack = recv_ack;
        f->tcp_flags = flags;
        f->data_off = (5 << 4);
        f->rx_win = tcp_wire_rcv_wnd(sk, flags);
        if (!(flags & RTE_TCP_SYN_FLAG))
                (void)tcp_options_apply_established(sk, f);
        return f;
}

/**
 * @brief Return the lcore responsible for TCP timers.
 *
 * rte_timer callbacks mutate the TCB and may retire it.  They must therefore
 * execute on the same packet worker that owns packet ingress and socket
 * commands; running them on the main I/O lcore would reintroduce the exact
 * close/free race the owner model is intended to remove.
 * @return Owning packet-worker lcore identifier.
 */
static unsigned int tcp_timer_lcore(const struct nsock *sk) {
        return sk->owner_lcore;
}

/** @brief Convert milliseconds to the DPDK timer cycle domain.
 * @param ms Duration in milliseconds.
 * @return Equivalent duration in timer cycles.
 */
static uint64_t tcp_ms_to_cycles(uint64_t ms) {
        return rte_get_timer_hz() * ms / 1000;
}

/**
 * @brief Choose an unused local TCP port from the ephemeral range.
 *
 * Round-robin pick an unused local TCP port in
 * [TCP_EPHEMERAL_PORT_MIN, TCP_EPHEMERAL_PORT_MAX].
 * Returns the port in network byte order, or 0 if the range is exhausted.
 * @return Available port in network byte order, or 0 when exhausted.
 */
static bool tcp_port_prediction_matches_owner(int predicted_queue,
                                              uint16_t owner_queue) {
        return predicted_queue < 0 || predicted_queue == owner_queue;
}

#ifdef TCP_TESTING
bool tcp_test_port_prediction_matches_owner(int predicted_queue,
                                            uint16_t owner_queue) {
        return tcp_port_prediction_matches_owner(predicted_queue, owner_queue);
}
#endif

static uint16_t tcp_alloc_ephemeral_port(const struct nsock *sk) {
        static uint16_t next[RTE_MAX_LCORE];
        unsigned int lcore_id = rte_lcore_id();
        uint16_t owner_queue;
        int predicted_queue;
        const uint32_t port_count =
            TCP_EPHEMERAL_PORT_MAX - TCP_EPHEMERAL_PORT_MIN + 1U;

        if (sk == NULL || lcore_id >= RTE_MAX_LCORE)
                return 0;
        /*
         * EAL seeds rte_rand() during startup.  Selecting a different initial
         * slot for every process avoids immediately reusing 49152 after an
         * abrupt traffic-generator restart, while later allocations retain
         * the deterministic round-robin scan and local collision check.
         */
        if (next[lcore_id] == 0)
                next[lcore_id] =
                    TCP_EPHEMERAL_PORT_MIN + (uint16_t)rte_rand_max(port_count);

        for (int i = 0;
             i < (TCP_EPHEMERAL_PORT_MAX - TCP_EPHEMERAL_PORT_MIN + 1); i++) {
                uint16_t p = next[lcore_id]++;
                /*
                 * next is uint16_t: incrementing 65535 wraps to zero, so the
                 * lower-bound check handles both wrap and normal cycling.
                 */
                if (next[lcore_id] < TCP_EPHEMERAL_PORT_MIN)
                        next[lcore_id] = TCP_EPHEMERAL_PORT_MIN;
                uint16_t be = htons(p);
                if (nsock_tcp_local_taken(g_net.local_ip, be))
                        continue;
                if (rx_dispatch_endpoint_is_registered(IPPROTO_TCP,
                                                       g_net.local_ip, be))
                        continue;
                if (stack_runtime_queue_for_lcore(sk->owner_lcore,
                                                  &owner_queue) == 0) {
                        predicted_queue = port_rss_queue_for_tcp(
                            sk->u.tcp.remote_ip, sk->local_ip,
                            sk->u.tcp.remote_port, be);
                        /*
                         * A single-queue port deliberately has no RSS state.
                         * A negative prediction means "no constraint", not a
                         * mismatch with owner queue zero.
                         */
                        if (!tcp_port_prediction_matches_owner(predicted_queue,
                                                               owner_queue))
                                continue;
                }
                return be;
        }
        return 0;
}

static void tcp_arm_syn_timer(struct nsock *sk, uint64_t delay_ms);
static void tcp_arm_data_rto(struct nsock *sk, uint64_t delay_ms);

/** Clear advisory sender SACK state on the socket's owner lcore. */
static void tcp_sack_score_clear(struct nsock *sk) {
        tcp_sack_state_reset(&sk->u.tcp, sk->u.tcp.snd_una);
}

/**
 * @brief Return the payload-only right edge already transmitted on this flight.
 *
 * SND.NXT may include FIN, while sndbuf contains only payload.  Taking the
 * earlier boundary prevents FIN sequence space from entering the scoreboard.
 */
static uint32_t tcp_sack_payload_flight_end(const struct nsock *sk) {
        uint32_t buffer_end =
            sk->u.tcp.sndbuf.head_seq + sk->u.tcp.sndbuf.len;

        return tcp_seq_lt(sk->u.tcp.sent_seq, buffer_end)
                   ? sk->u.tcp.sent_seq
                   : buffer_end;
}

#ifdef TCP_TESTING
void tcp_test_sack_score_update(struct nsock *sk,
                                const struct tcp_sack_block *blocks,
                                uint8_t count) {
        (void)tcp_sack_update(&sk->u.tcp, sk->u.tcp.snd_una, blocks, count,
                              tcp_sack_payload_flight_end(sk));
}

bool tcp_test_sack_schedule_retransmit(struct nsock *sk) {
        uint32_t end = tcp_sack_payload_flight_end(sk);

        if (tcp_sack_score_count(&sk->u.tcp) == 0)
                return false;
        tcp_sack_enter_recovery(&sk->u.tcp, TCP_RECOVERY_SACK, end);
        return sk->u.tcp.sack.pending.kind != TCP_RECOVERY_TX_NONE;
}

void tcp_test_sack_score_clear(struct nsock *sk) {
        tcp_sack_score_clear(sk);
}
#endif

/**
 * @brief Handle a per-TCB retransmission or TIME_WAIT timer expiry.
 *
 * One rte_timer is multiplexed by @c status:
 *
 *   SYN_SENT      -- retransmit SYN (same ISN); give up -> CLOSED + wake
 * connect SYN_RECV      -- retransmit SYN+ACK (same ISN/ack); give up -> free
 * child ESTABLISHED / CLOSE_WAIT -- explicit-sequence data RTO recovery
 * FIN_WAIT_1 / LAST_ACK / CLOSING -- payload recovery followed by FIN RTO
 * TIME_WAIT -- 2MSL expiry -> CLOSED (+ signal or free orphan)
 *
 * Control segments (SYN / SYN+ACK / FIN) live on send_buf and are freed after
 * TX, so RTO rebuilds them via tcp_make_fragment + tcp_enqueue_fragment.
 * App data stays in sndbuf until ACK; every retransmission uses a separate
 * candidate and therefore never rewinds snd_nxt.
 * Timer callbacks execute on the socket's owner lcore, serialized with
 * command handling, packet ingress, ACK processing, and TX flushing.
 * @param timer Expired DPDK timer (unused).
 * @param arg Owning @ref nsock.
 */
static void tcp_timer_cb(__attribute__((unused)) struct rte_timer *timer,
                         void *arg) {
        struct nsock *sk = (struct nsock *)arg;
        if (sk->u.tcp.status == TCP_STATUS_SYN_SENT) {
                /* Active-open SYN RTO.  Completion wakes the parked CONNECT
                 * command; the application may then close the still-published
                 * descriptor. */
                if (sk->u.tcp.retries < TCP_SYN_MAX_RETRIES) {
                        sk->u.tcp.retries++;
                        sk->u.tcp.syn_retransmitted = true;
                        struct tcp_fragment *syn_f = tcp_make_fragment(
                            sk, RTE_TCP_SYN_FLAG, sk->u.tcp.sent_seq, 0);
                        if (syn_f != NULL)
                                (void)tcp_options_apply_syn(sk, syn_f, true,
                                                            true, true, 0);
                        /*
                         * The timer for the next retry backs off from the
                         * fixed initial SYN RTO: 1s, 2s, 4s, and so on.
                         */
                        uint64_t delay_ms = (uint64_t)TCP_SYN_RTO_MS
                                            << (sk->u.tcp.retries - 1);
                        if (tcp_enqueue_fragment(sk, syn_f) == 0) {
                                LOG_TCP_INFO(TCP_SK_FMT
                                             " event=rto-retransmit kind=syn "
                                             "retry=%u next_rto_ms=%" PRIu64,
                                             TCP_SK_ARG(sk), sk->u.tcp.retries,
                                             delay_ms);
                        }
                        tcp_arm_syn_timer(sk, delay_ms);
                        return;
                }

                LOG_TCP_WARN(TCP_SK_FMT " event=rto-give-up kind=syn",
                             TCP_SK_ARG(sk));
                tcp_stream_set_status(sk, TCP_STATUS_CLOSED);
                tcp_drain_send(sk);
                socket_owner_complete_connect(sk, ETIMEDOUT);
                socket_owner_ready_post(sk, OWNER_IO_EV_ERROR);
                if (sk->app_closed) {
                        tcp_drain_recv(sk);
                        nsock_free(sk);
                }
        } else if (sk->u.tcp.status == TCP_STATUS_TIME_WAIT) {
                tcp_stream_set_status(sk, TCP_STATUS_CLOSED);
                tcp_drain_recv(sk);
                tcp_drain_send(sk);
                /*
                 * fd numbers are no longer object identity.  app_visible
                 * distinguishes an accepted socket from an orphan child, and
                 * app_closed records that the published fd was detached.
                 */
                if (!sk->app_visible || sk->app_closed)
                        nsock_free(sk);
        } else if (sk->u.tcp.status == TCP_STATUS_ESTABLISHED ||
                   sk->u.tcp.status == TCP_STATUS_CLOSE_WAIT) {
                /* Data-only RTO. FIN states are handled below. */
                if (sk->u.tcp.snd_una == sk->u.tcp.sent_seq) {
                        return; /* nothing in flight: ignore */
                }
                if (sk->u.tcp.retries >= TCP_DATA_MAX_RETRIES) {
                        LOG_TCP_WARN(TCP_SK_FMT
                                     " event=rto-give-up kind=data una=%u "
                                     "nxt=%u retry=%u",
                                     TCP_SK_ARG(sk), sk->u.tcp.snd_una,
                                     sk->u.tcp.sent_seq, sk->u.tcp.retries);
                        /* Optional: RST / CLOSED; minimal: stop retrying */
                        return;
                }
                sk->u.tcp.retries++;
                /*
                 * Data RTO uses the per-stream estimator.  This also drops
                 * the outstanding RTT probe before retransmission.
                 */
                tcp_rtt_on_timeout(sk);
                uint32_t flight_end =
                    tcp_sack_payload_flight_end(sk);
                uint32_t flight_size = flight_end - sk->u.tcp.snd_una;
                bool first_rto = !sk->u.tcp.cc.rto_loss_valid ||
                                 sk->u.tcp.cc.rto_loss_seq !=
                                     sk->u.tcp.snd_una;
                struct tcp_cc_loss_event loss = {
                    .reason = TCP_CC_LOSS_RTO,
                    .flight_size = flight_size,
                    .recovery_point = flight_end,
                    .first_rto_for_seq = first_rto,
                };
                sk->u.tcp.cc.rto_loss_valid = true;
                sk->u.tcp.cc.rto_loss_seq = sk->u.tcp.snd_una;
                tcp_cc_on_rto(&sk->u.tcp, &loss);
                tcp_sack_on_rto(&sk->u.tcp, flight_end);
                LOG_TCP_INFO(TCP_SK_FMT
                             " event=rto-retransmit kind=data retry=%u "
                             "range=%u:%u rto_ms=%u",
                             TCP_SK_ARG(sk), sk->u.tcp.retries,
                             sk->u.tcp.sack.pending.seq,
                             sk->u.tcp.sack.pending.end,
                             sk->u.tcp.rto_ms);
                tcp_arm_data_rto(sk, sk->u.tcp.rto_ms);
                tcp_mark_tx_dirty(sk);
        } else if (sk->u.tcp.status == TCP_STATUS_SYN_RECV) {
                /*
                 * Passive-open SYN+ACK RTO. Mirror of SYN_SENT: the first
                 * SYN+ACK was freed after TX, so rebuild with the same ISN
                 * (sent_seq) and ack (recv_ack). Exponential backoff.
                 */
                if (sk->u.tcp.retries < TCP_SYN_MAX_RETRIES) {
                        sk->u.tcp.retries++;
                        sk->u.tcp.syn_retransmitted = true;
                        struct tcp_fragment *syn_ack_f = tcp_make_fragment(
                            sk, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG,
                            sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
                        (void)tcp_options_apply_syn(
                            sk, syn_ack_f, sk->u.tcp.wscale_ok,
                            sk->u.tcp.timestamps_ok,
                            sk->u.tcp.sack_local_offered,
                            sk->u.tcp.ts_recent);
                        uint64_t delay_ms = (uint64_t)TCP_SYN_RTO_MS
                                            << (sk->u.tcp.retries - 1);
                        if (tcp_enqueue_fragment(sk, syn_ack_f) == 0) {
                                LOG_TCP_INFO(TCP_SK_FMT
                                             " event=rto-retransmit "
                                             "kind=syn-ack retry=%u seq=%u "
                                             "ack=%u next_rto_ms=%" PRIu64,
                                             TCP_SK_ARG(sk), sk->u.tcp.retries,
                                             sk->u.tcp.sent_seq,
                                             sk->u.tcp.recv_ack, delay_ms);
                        }
                        tcp_arm_syn_timer(sk, delay_ms);
                        return;
                }
                /* Give up: release half-open child and backlog credit. */
                LOG_TCP_WARN(TCP_SK_FMT " event=rto-give-up kind=syn-ack",
                             TCP_SK_ARG(sk));
                if (sk->u.tcp.listener != NULL &&
                    sk->u.tcp.listener->u.tcp.syn_pending > 0) {
                        sk->u.tcp.listener->u.tcp.syn_pending--;
                }
                rte_timer_stop(&sk->u.tcp.timer);
                tcp_drain_recv(sk);
                tcp_drain_send(sk);
                nsock_free(sk);
        } else if (sk->u.tcp.status == TCP_STATUS_FIN_WAIT_1 ||
                   sk->u.tcp.status == TCP_STATUS_LAST_ACK ||
                   sk->u.tcp.status == TCP_STATUS_CLOSING) {
                /*
                 * FIN control-segment RTO.
                 *
                 * After tcp_close, invariant is usually:
                 *   snd_una ..[sndbuf unacked data].. fin_seq .. sent_seq
                 * with sent_seq == fin_seq + 1. FIN itself is not in sndbuf.
                 *
                 * Payload, when present, is retransmitted by explicit
                 * sequence without touching snd_nxt. FIN-only loss continues
                 * to rebuild the control fragment at snd_una.
                 */
                if (sk->u.tcp.snd_una == sk->u.tcp.sent_seq) {
                        return; /* FIN (and data) already ACKed */
                }

                if (sk->u.tcp.retries >= TCP_DATA_MAX_RETRIES) {
                        LOG_TCP_WARN(TCP_SK_FMT
                                     " event=rto-give-up kind=fin una=%u "
                                     "nxt=%u retry=%u",
                                     TCP_SK_ARG(sk), sk->u.tcp.snd_una,
                                     sk->u.tcp.sent_seq, sk->u.tcp.retries);
                        return;
                }
                sk->u.tcp.retries++;
                tcp_rtt_on_timeout(sk);

                bool retransmit_data = false;
                if (sk->u.tcp.sndbuf.len > 0) {
                        uint32_t flight_end =
                            tcp_sack_payload_flight_end(sk);
                        uint32_t flight_size =
                            flight_end - sk->u.tcp.snd_una;
                        bool first_rto = !sk->u.tcp.cc.rto_loss_valid ||
                                         sk->u.tcp.cc.rto_loss_seq !=
                                             sk->u.tcp.snd_una;
                        struct tcp_cc_loss_event loss = {
                            .reason = TCP_CC_LOSS_RTO,
                            .flight_size = flight_size,
                            .recovery_point = flight_end,
                            .first_rto_for_seq = first_rto,
                        };
                        sk->u.tcp.cc.rto_loss_valid = true;
                        sk->u.tcp.cc.rto_loss_seq = sk->u.tcp.snd_una;
                        tcp_cc_on_rto(&sk->u.tcp, &loss);
                        tcp_sack_on_rto(&sk->u.tcp, flight_end);
                        retransmit_data = true;
                        LOG_TCP_INFO(
                            TCP_SK_FMT
                            " event=rto-retransmit kind=fin-data "
                            "retry=%u range=%u:%u rto_ms=%u",
                            TCP_SK_ARG(sk), sk->u.tcp.retries,
                            sk->u.tcp.sack.pending.seq,
                            sk->u.tcp.sack.pending.end,
                            sk->u.tcp.rto_ms);
                } else {
                        struct tcp_fragment *fin_f = tcp_make_fragment(
                            sk, RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG,
                            sk->u.tcp.snd_una, sk->u.tcp.recv_ack);
                        if (tcp_enqueue_fragment(sk, fin_f) == 0) {
                                LOG_TCP_INFO(
                                    TCP_SK_FMT " event=rto-retransmit kind=fin "
                                               "retry=%u seq=%u rto_ms=%u",
                                    TCP_SK_ARG(sk), sk->u.tcp.retries,
                                    sk->u.tcp.snd_una, sk->u.tcp.rto_ms);
                        }
                }

                tcp_arm_data_rto(sk, sk->u.tcp.rto_ms);
                if (retransmit_data)
                        tcp_mark_tx_dirty(sk);
        }
}

/** @brief Arm the data or FIN retransmission timer.
 * @param sk Stream whose timer is armed.
 * @param delay_ms Expiry delay in milliseconds.
 */
static void tcp_arm_data_rto(struct nsock *sk, uint64_t delay_ms) {
        rte_timer_reset(&sk->u.tcp.timer, tcp_ms_to_cycles(delay_ms), SINGLE,
                        tcp_timer_lcore(sk), tcp_timer_cb, sk);
}

/** Owner-local implementation used to commit a peer-window update. */
static bool tcp_update_snd_wnd_local(struct nsock *sk, uint32_t seg_seq,
                                     uint32_t seg_ack, uint16_t seg_wnd,
                                     uint8_t scale, bool *need_tx);

/**
 * @brief Atomically process ACK, window, SACK, recovery, RTT, and CC state.
 *
 * ACKs in [SND.UNA, SND.NXT] are accepted.  ACK == SND.UNA retains sndbuf but
 * may add new SACK information and trigger fast recovery.  Cumulative progress
 * removes only payload bytes, clips the scoreboard, updates RTT/RTO state, and
 * dispatches one coherent event to congestion control as one owner-lcore
 * operation.
 *
 * @param sk Stream receiving the ACK.
 * @param ack Packet ACK field in host order.
 * @param classic_dup_candidate Packet met the non-SACK duplicate-ACK shape.
 * @param apply_window Whether this packet carries an advertised window update.
 * @param seg_seq Packet SEG.SEQ used for RFC 793 window-update ordering.
 * @param seg_wnd Unscaled 16-bit advertised window in host order.
 * @param scale Negotiated shift used to decode @p seg_wnd.
 */
static void tcp_process_peer_ack_ex(struct nsock *sk, uint32_t ack,
                                    bool classic_dup_candidate,
                                    bool apply_window, uint32_t seg_seq,
                                    uint16_t seg_wnd, uint8_t scale) {
        bool window_need_tx = false;
        bool window_accepted = false;

        /* Duplicate ACKs at snd_una may still carry useful SACK blocks. */
        if (tcp_seq_lt(ack, sk->u.tcp.snd_una) ||
            tcp_seq_gt(ack, sk->u.tcp.sent_seq)) {
                LOG_TCP_TRACE(
                    TCP_SK_FMT " event=peer-ack-ignored ack=%u una=%u nxt=%u",
                    TCP_SK_ARG(sk), ack, sk->u.tcp.snd_una, sk->u.tcp.sent_seq);
                return;
        }
        window_accepted =
            apply_window && tcp_update_snd_wnd_local(
                                sk, seg_seq, ack, seg_wnd, scale,
                                &window_need_tx);

        uint32_t old_una = sk->u.tcp.snd_una;
        uint32_t old_sndbuf_len = sk->u.tcp.sndbuf.len;
        uint32_t flight_end = tcp_sack_payload_flight_end(sk);
        uint32_t flight_size = flight_end - sk->u.tcp.snd_una;
        uint32_t acked = 0;
        bool progressed = ack != sk->u.tcp.snd_una;
        bool rtt_sampled = false;
        uint32_t now_ms = tcp_options_now_ms();
        uint32_t raw_rtt_ms = 0;
        bool raw_rtt_valid = false;
        enum tcp_recovery_mode old_mode = sk->u.tcp.sack.mode;

        if (progressed) {
                acked = ack - sk->u.tcp.snd_una; /* modulo-2^32 */
                if (acked > sk->u.tcp.sndbuf.len)
                        acked = sk->u.tcp.sndbuf.len;
                tcp_sndbuf_remove(&sk->u.tcp.sndbuf, acked);
                sk->u.tcp.snd_una = ack;

                /* ACK progress only: duplicate ACKs never create RTT samples. */
                raw_rtt_valid = tcp_rtt_sample_ack(
                    sk, sk->u.tcp.rx_timestamp_present,
                    sk->u.tcp.rx_tsecr, now_ms, &raw_rtt_ms);
                rtt_sampled = tcp_rtt_on_ack(
                    sk, ack, sk->u.tcp.rx_timestamp_present,
                    sk->u.tcp.rx_tsecr);
        }

        struct tcp_sack_ack_result sack_result = tcp_sack_update(
            &sk->u.tcp, ack, sk->u.tcp.rx_sacks,
            sk->u.tcp.sack_permitted ? sk->u.tcp.rx_sack_count : 0,
            flight_end);

        if (sack_result.dsack_valid)
                tcp_cc_on_dsack(&sk->u.tcp, &sack_result.dsack,
                                sack_result.dsack_covers_retransmission);
        bool recovery_exited = sack_result.recovery_exited;
        bool newreno_partial = false;
        if (old_mode == TCP_RECOVERY_NEWRENO && progressed) {
                if (!tcp_seq_lt(ack, sk->u.tcp.sack.recovery_point)) {
                        sk->u.tcp.sack.mode = TCP_RECOVERY_NORMAL;
                        sk->u.tcp.sack.dup_acks = 0;
                        tcp_sack_cancel_candidate(&sk->u.tcp);
                        recovery_exited = true;
                } else {
                        /* RFC 6582 keeps recovery active and immediately
                         * retransmits the new lowest unacknowledged segment. */
                        newreno_partial = true;
                        (void)tcp_sack_schedule_newreno_partial(
                            &sk->u.tcp, flight_end);
                }
        }

        if (progressed)
                sk->u.tcp.sack.dup_acks = 0;

        bool entered_recovery = false;
        if (sk->u.tcp.sack_permitted &&
            sack_result.new_sack_information &&
            sk->u.tcp.sack.mode == TCP_RECOVERY_NORMAL) {
                sk->u.tcp.sack.dup_acks++;
                if (sk->u.tcp.sack.dup_acks >= TCP_SACK_DUP_THRESH ||
                    tcp_sack_is_lost(&sk->u.tcp, sk->u.tcp.snd_una)) {
                        struct tcp_cc_loss_event loss = {
                            .reason = TCP_CC_LOSS_SACK,
                            .flight_size = flight_size,
                            .recovery_point = flight_end,
                            .first_rto_for_seq = false,
                        };
                        tcp_cc_on_loss(&sk->u.tcp, &loss);
                        tcp_sack_enter_recovery(&sk->u.tcp,
                                                TCP_RECOVERY_SACK,
                                                flight_end);
                        tcp_sack_set_pipe(&sk->u.tcp, flight_end);
                        entered_recovery = true;
                } else {
                        /* RFC 6675 Limited Transmit uses the reduced pipe
                         * estimate exposed by new SACK information. */
                        sk->u.tcp.sack.limited_transmit = true;
                        tcp_sack_set_pipe(&sk->u.tcp, flight_end);
                }
        } else if (!sk->u.tcp.sack_permitted && !progressed &&
                   classic_dup_candidate &&
                   tcp_seq_lt(sk->u.tcp.snd_una, flight_end)) {
                if (sk->u.tcp.sack.mode == TCP_RECOVERY_NORMAL) {
                        sk->u.tcp.sack.dup_acks++;
                        if (sk->u.tcp.sack.dup_acks >=
                            TCP_SACK_DUP_THRESH) {
                                struct tcp_cc_loss_event loss = {
                                    .reason = TCP_CC_LOSS_DUPACK,
                                    .flight_size = flight_size,
                                    .recovery_point = flight_end,
                                    .first_rto_for_seq = false,
                                };
                                tcp_cc_on_loss(&sk->u.tcp, &loss);
                                tcp_sack_enter_recovery(
                                    &sk->u.tcp,
                                    TCP_RECOVERY_NEWRENO, flight_end);
                                entered_recovery = true;
                        }
                }
        }

        if (sk->u.tcp.sack.mode == TCP_RECOVERY_SACK) {
                tcp_sack_set_pipe(&sk->u.tcp, flight_end);
        } else if (sk->u.tcp.sack.mode == TCP_RECOVERY_RTO &&
                   (progressed || sack_result.new_sack_information)) {
                (void)tcp_sack_schedule_next(&sk->u.tcp,
                                             sk->u.tcp.sndbuf.head_seq +
                                                 sk->u.tcp.sndbuf.len,
                                             false);
        }

        struct tcp_cc_ack_event cc_ack = {
            .ack_seq = ack,
            .snd_nxt = flight_end,
            .acked_bytes = acked,
            .newly_sacked_bytes = sack_result.newly_sacked_bytes,
            .flight_size = flight_size,
            .now_ms = now_ms,
            .rtt_sample_ms = raw_rtt_ms,
            .rtt_sample_valid = raw_rtt_valid,
            .cwnd_limited = sk->u.tcp.cc.cwnd_limited,
            .duplicate_ack = !progressed &&
                             (classic_dup_candidate ||
                              sack_result.new_sack_information),
            .entered_recovery = entered_recovery,
            .partial_ack = newreno_partial,
            .newreno_recovery =
                old_mode == TCP_RECOVERY_NEWRENO ||
                (entered_recovery &&
                 sk->u.tcp.sack.mode == TCP_RECOVERY_NEWRENO),
            .in_recovery = old_mode != TCP_RECOVERY_NORMAL ||
                           entered_recovery,
        };
        tcp_cc_on_ack(&sk->u.tcp, &cc_ack);
        if (recovery_exited)
                tcp_cc_on_recovery_exit(&sk->u.tcp);

        if (progressed && sk->u.tcp.snd_una == sk->u.tcp.sent_seq) {
                /* Nothing in flight (data and/or FIN ACKed): stop RTO.
                 * Do not touch timers owned by other states. */
#ifndef TCP_TESTING
                if (sk->u.tcp.status != TCP_STATUS_TIME_WAIT &&
                    sk->u.tcp.status != TCP_STATUS_SYN_SENT &&
                    sk->u.tcp.status != TCP_STATUS_SYN_RECV)
                        rte_timer_stop(&sk->u.tcp.timer);
#endif
                sk->u.tcp.retries = 0;
                tcp_rtt_on_flight_acked(sk);
        } else if (progressed) {
                /* Partial ACK advanced una; restart RTO for remaining flight.
                 */
                sk->u.tcp.retries = 0;
#ifndef TCP_TESTING
                tcp_arm_data_rto(sk, sk->u.tcp.rto_ms);
#endif
        }

        LOG_TCP_DEBUG(TCP_SK_FMT " event=peer-ack ack=%u una=%u->%u acked=%u "
                                 "sndbuf=%u->%u sacks=%u rto=%s "
                                 "rtt_sample=%u rto_ms=%u",
                      TCP_SK_ARG(sk), ack, old_una, sk->u.tcp.snd_una, acked,
                      old_sndbuf_len, sk->u.tcp.sndbuf.len,
                      tcp_sack_score_count(&sk->u.tcp),
                      !progressed ? "unchanged"
                      : sk->u.tcp.snd_una == sk->u.tcp.sent_seq ? "stopped"
                                                                : "rearmed",
                      rtt_sampled, sk->u.tcp.rto_ms);
        /*
         * Only re-arm the dirty TX queue when unsent payload remains.  Pure
         * ACK progress with an empty send cursor does not create TX work.
         */
        bool need_tx = false;
        if (sk->u.tcp.sack.pending.kind != TCP_RECOVERY_TX_NONE ||
            sk->u.tcp.sack.mode == TCP_RECOVERY_SACK ||
            sk->u.tcp.sack.limited_transmit)
                need_tx = true;
        if (sk->u.tcp.sndbuf.len > 0) {
                uint32_t buf_end =
                    sk->u.tcp.sndbuf.head_seq + sk->u.tcp.sndbuf.len;
                need_tx = need_tx ||
                          tcp_seq_lt(sk->u.tcp.sent_seq, buf_end);
        }
        if (progressed || window_accepted) {
                /* Retry commands parked by zero local/peer send-window
                 * space. */
#ifndef TCP_TESTING
                socket_owner_wake_send(sk);
                socket_owner_ready_post(sk, OWNER_IO_EV_WRITE);
#endif
        }
        if (need_tx || window_need_tx)
                tcp_mark_tx_dirty(sk);
}

/** Process an ACK in states that do not consume its window advertisement. */
static void tcp_process_peer_ack(struct nsock *sk, uint32_t ack) {
        tcp_process_peer_ack_ex(sk, ack, false, false, 0, 0, 0);
}

#ifdef TCP_TESTING
void tcp_test_process_peer_ack(struct nsock *sk, uint32_t ack,
                               bool classic_duplicate) {
        tcp_process_peer_ack_ex(sk, ack, classic_duplicate, false, 0, 0, 0);
}
#endif

/**
 * @brief Apply an RFC 793-ordered peer send-window update.
 *
 * Older advertisements cannot overwrite a newer window.  When an update is
 * accepted, waiting application senders are awakened so each can re-evaluate
 * its admission limit when the owner retries its parked command.
 *
 * @param sk TCP socket whose peer window is updated.
 * @param seg_seq SEG.SEQ from the segment carrying the update.
 * @param seg_ack SEG.ACK from the segment carrying the update.
 * @param seg_wnd Advertised receive window field in host byte order.
 * @param scale Window Scale announced by the peer; zero before negotiation.
 * @param need_tx Set when unsent sndbuf payload may now be transmitted.
 * @return True when this advertisement was new enough to be accepted.
 */
static bool tcp_update_snd_wnd_local(struct nsock *sk, uint32_t seg_seq,
                                     uint32_t seg_ack, uint16_t seg_wnd,
                                     uint8_t scale, bool *need_tx) {
        struct tcp_stream *tp = &sk->u.tcp;
        uint32_t scaled_wnd = (uint32_t)seg_wnd << scale;

        /*
         * RFC 793 window-update ordering: an older segment cannot overwrite
         * the newest accepted advertised window.
         */
        if (!tp->snd_wnd_valid || tcp_seq_lt(tp->snd_wl1, seg_seq) ||
            (tp->snd_wl1 == seg_seq && tcp_seq_leq(tp->snd_wl2, seg_ack))) {
                uint32_t old_wnd = tp->snd_wnd;
                tp->snd_wnd = scaled_wnd;
                tp->snd_wl1 = seg_seq;
                tp->snd_wl2 = seg_ack;
                tp->snd_wnd_valid = true;
                if (need_tx != NULL)
                        *need_tx = false;
                if (tp->sndbuf.len > 0) {
                        uint32_t buf_end = tp->sndbuf.head_seq + tp->sndbuf.len;
                        if (need_tx != NULL)
                                *need_tx = tcp_seq_lt(tp->sent_seq, buf_end);
                }
                LOG_TCP_DEBUG(TCP_SK_FMT
                              " event=peer-window accepted wire=%u scale=%u "
                              "wnd=%u->%u seg_seq=%u seg_ack=%u",
                              TCP_SK_ARG(sk), seg_wnd, scale, old_wnd,
                              scaled_wnd, seg_seq, seg_ack);
        } else {
                LOG_TCP_TRACE(TCP_SK_FMT
                              " event=peer-window-ignored wire=%u scale=%u "
                              "decoded=%u seg_seq=%u seg_ack=%u",
                              TCP_SK_ARG(sk), seg_wnd, scale, scaled_wnd,
                              seg_seq, seg_ack);
                return false;
        }
        return true;
}

/** Apply a standalone peer-window update and wake blocked senders. */
static void tcp_update_snd_wnd(struct nsock *sk, uint32_t seg_seq,
                               uint32_t seg_ack, uint16_t seg_wnd,
                               uint8_t scale) {
        bool need_tx = false;

        bool accepted = tcp_update_snd_wnd_local(
            sk, seg_seq, seg_ack, seg_wnd, scale, &need_tx);
        if (!accepted)
                return;
        /* Harmless if no sender is parked or the accepted window stayed zero.
         */
#ifndef TCP_TESTING
        socket_owner_wake_send(sk);
#endif
        socket_owner_ready_post(sk, OWNER_IO_EV_WRITE);
        if (need_tx)
                tcp_mark_tx_dirty(sk);
}

#ifdef TCP_TESTING
void tcp_test_update_snd_wnd(struct nsock *sk, uint32_t seg_seq,
                             uint32_t seg_ack, uint16_t seg_wnd) {
        tcp_update_snd_wnd(sk, seg_seq, seg_ack, seg_wnd, 0);
}
#endif

/** @brief Arm the SYN or SYN+ACK retransmission timer.
 * @param sk Stream whose timer is armed.
 * @param delay_ms Expiry delay in milliseconds.
 */
static void tcp_arm_syn_timer(struct nsock *sk, uint64_t delay_ms) {
        rte_timer_reset(&sk->u.tcp.timer, tcp_ms_to_cycles(delay_ms), SINGLE,
                        tcp_timer_lcore(sk), tcp_timer_cb, sk);
}

/** @brief Enter TIME_WAIT and arm the 2MSL expiry timer.
 * @param sk Stream that completed active close processing.
 */
static void tcp_enter_time_wait(struct nsock *sk) {
        tcp_stream_set_status(sk, TCP_STATUS_TIME_WAIT);
        sk->u.tcp.retries = 0;
        rte_timer_reset(&sk->u.tcp.timer, tcp_ms_to_cycles(TCP_2MSL_MS), SINGLE,
                        tcp_timer_lcore(sk), tcp_timer_cb, sk);
}

/**
 * @brief Handle a bare SYN received by a listening socket.
 *
 * Passive open step 1: new SYN for @p listener.
 * Creates a SYN_RECV child (no fd), queues SYN+ACK on the child send_buf,
 * and arms the SYN_RECV RTO (same timer as active-open SYN). The SYN+ACK
 * fragment is freed after TX; on timeout tcp_timer_cb rebuilds it.
 * Does NOT enqueue onto accept_queue -- that happens only after the final ACK.
 * @param listener Listening socket.
 * @param hdr Inbound TCP header.
 * @param eth Ethernet header; its source MAC is used for a direct RST reply.
 * @param iphdr IPv4 header; provides the tuple and SEG.LEN for an RST reply.
 * @param remote_ip Peer IPv4 address in network byte order.
 * @param remote_port Peer TCP port in network byte order.
 * @param peer_mss MSS announced by the peer SYN, or the local default.
 * @return 0; ingress retains ownership of the packet mbuf.
 */
static int tcp_state_listen(struct nsock *listener, struct rte_tcp_hdr *hdr,
                            const struct rte_ether_hdr *eth,
                            const struct rte_ipv4_hdr *iphdr,
                            uint32_t remote_ip, uint16_t remote_port,
                            const struct tcp_options_rx *opts) {
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
                tcp_send_reset_reply(eth, iphdr, hdr);
                LOG_WARN("tcp backlog full listener_" TCP_ID_FMT
                         " syn_pending=%u "
                         "accepted=%u backlog=%u; drop SYN from " IP_FMT ":%u",
                         TCP_ID_ARG(listener), listener->u.tcp.syn_pending,
                         accepted, listener->u.tcp.backlog, IP_ARG(remote_ip),
                         rte_be_to_cpu_16(remote_port));
                return 0;
        }

        struct nsock *child = tcp_stream_create(
            remote_ip, listener->local_ip, remote_port, listener->local_port);
        if (child == NULL) {
                tcp_send_reset_reply(eth, iphdr, hdr);
                LOG_ERROR("tcp stream create failed listener_" TCP_ID_FMT
                          " peer " IP_FMT ":%u",
                          TCP_ID_ARG(listener), IP_ARG(remote_ip),
                          rte_be_to_cpu_16(remote_port));
                return 0;
        }
        /* Used by tcp_state_syn_recv to wake naccept on the parent. */
        child->u.tcp.listener = listener;
        tcp_options_negotiate_syn(child, opts);

        listener->u.tcp.syn_pending++;

        struct tcp_fragment *f = tcp_fragment_alloc();
        if (f == NULL) {
                tcp_stream_set_status(child, TCP_STATUS_CLOSED);
                nsock_free(child);
                listener->u.tcp.syn_pending--;
                return 0;
        }
        f->src_port = child->local_port;
        f->dst_port = child->u.tcp.remote_port;
        f->sent_seq = child->u.tcp.sent_seq;
        f->recv_ack = ntohl(hdr->sent_seq) + 1;
        f->tcp_flags = RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG;
        f->rx_win = tcp_wire_rcv_wnd(child, f->tcp_flags);
        (void)tcp_options_apply_syn(child, f, child->u.tcp.wscale_ok,
                                    child->u.tcp.timestamps_ok,
                                    true,
                                    child->u.tcp.ts_recent);
        child->u.tcp.recv_ack = f->recv_ack;
        if (tcp_enqueue_fragment(child, f) != 0) {
                tcp_stream_set_status(child, TCP_STATUS_CLOSED);
                nsock_free(child);
                listener->u.tcp.syn_pending--;
                return 0;
        }
        /* Independent SYN+ACK RTO; stopped when the final ACK arrives. */
        child->u.tcp.retries = 0;
        tcp_arm_syn_timer(child, TCP_SYN_RTO_MS);

        LOG_INFO("tcp handshake [1/3] SYN rx " IP_FMT ":%u -> " IP_FMT
                 ":%u seq=%u; reply SYN+ACK seq=%u ack=%u syn_pending=%u",
                 IP_ARG(remote_ip), rte_be_to_cpu_16(remote_port),
                 IP_ARG(listener->local_ip),
                 rte_be_to_cpu_16(listener->local_port), ntohl(hdr->sent_seq),
                 child->u.tcp.sent_seq, child->u.tcp.recv_ack,
                 listener->u.tcp.syn_pending);
        return 0;
}

/**
 * @brief Handle a SYN+ACK during active open.
 *
 * Active open step 2/3: SYN_SENT --recv SYN+ACK, send ACK--> ESTABLISHED.
 * Validates that the peer ACK covers our ISN (sent_seq + 1), then wakes the
 * blocking tcp_connect waiter. Timer is stopped so RTO cannot race the ACK.
 * @param sk SYN_SENT stream.
 * @param hdr Inbound TCP header.
 * @param mbuf Inbound packet; not retained.
 * @return 0; ingress frees @p mbuf.
 */
static int tcp_state_syn_sent(struct nsock *sk, struct rte_tcp_hdr *hdr,
                              struct rte_mbuf *mbuf) {
        (void)mbuf;
        if (sk->u.tcp.status != TCP_STATUS_SYN_SENT)
                return 0;

        /*
         * Only SYN+ACK advances the handshake. tcp_ingress validates and
         * handles RST before dispatching to this state handler.
         */
        if (!(hdr->tcp_flags & RTE_TCP_SYN_FLAG) ||
            !(hdr->tcp_flags & RTE_TCP_ACK_FLAG))
                return 0;

        uint32_t acknum = ntohl(hdr->recv_ack);
        /* sent_seq still holds the ISN until we consume the SYN below. */
        uint32_t expect = sk->u.tcp.sent_seq + 1;
        if (acknum != expect) {
                LOG_WARN(
                    "tcp handshake ACK mismatch " IP_FMT ":%u ack=%u expect=%u",
                    IP_ARG(sk->u.tcp.remote_ip),
                    rte_be_to_cpu_16(sk->u.tcp.remote_port), acknum, expect);
                return 0;
        }

        sk->u.tcp.recv_ack = ntohl(hdr->sent_seq) + 1;
        sk->u.tcp.sent_seq += 1; /* consume our SYN in the sequence space */

        /* Reset sndbuf and snd_una after handshake completion. */
        sk->u.tcp.snd_una = sk->u.tcp.sent_seq;
        tcp_sndbuf_reset(&sk->u.tcp.sndbuf, sk->u.tcp.sent_seq);
        tcp_sack_state_reset(&sk->u.tcp, sk->u.tcp.sent_seq);

        struct tcp_fragment *ack_f = tcp_make_fragment(
            sk, RTE_TCP_ACK_FLAG, sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
        if (tcp_enqueue_fragment(sk, ack_f) != 0) {
                /* Stay SYN_SENT; RTO may retry SYN. Do not wake connect yet. */
                return 0;
        }

        rte_timer_stop(&sk->u.tcp.timer);
        tcp_update_snd_wnd(sk, ntohl(hdr->sent_seq), ntohl(hdr->recv_ack),
                           ntohs(hdr->rx_win), 0);
        tcp_rtt_reset(sk);
        tcp_cc_init_default(&sk->u.tcp, sk->u.tcp.syn_retransmitted);
        tcp_stream_set_status(sk, TCP_STATUS_ESTABLISHED);

        LOG_TCP_INFO("tcp handshake done (active) " TCP_ID_FMT " peer " IP_FMT
                     ":%u",
                     TCP_ID_ARG(sk), IP_ARG(sk->u.tcp.remote_ip),
                     rte_be_to_cpu_16(sk->u.tcp.remote_port));

        /* Complete the CONNECT command parked by tcp_connect(). */
        socket_owner_complete_connect(sk, 0);
        socket_owner_ready_post(sk, OWNER_IO_EV_CONNECTED);
        return 0;
}

/** @brief Handle SYN retransmissions or the final ACK in SYN_RECV.
 *
 * RST validation and teardown are centralized in tcp_ingress.
 * @param sk Passive-open child stream.
 * @param hdr Inbound TCP header.
 * @param mbuf Inbound packet; not retained.
 * @return 0; ingress frees @p mbuf.
 */
static int tcp_state_syn_recv(struct nsock *sk, struct rte_tcp_hdr *hdr,
                              struct rte_mbuf *mbuf) {
        if (sk->u.tcp.status != TCP_STATUS_SYN_RECV)
                return 0;

        /*
         * Peer lost our SYN+ACK and retransmitted SYN: resend SYN+ACK with the
         * same ISN / ack. Do not allocate another child (4-tuple already maps
         * here via tcp_stream_search). Reset retries and re-arm RTO so a
         * late peer SYN does not keep the previous exponential backoff.
         */
        if ((hdr->tcp_flags & RTE_TCP_SYN_FLAG) &&
            !(hdr->tcp_flags & RTE_TCP_ACK_FLAG)) {
                struct tcp_fragment *f =
                    tcp_make_fragment(sk, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG,
                                      sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
                if (f != NULL)
                        (void)tcp_options_apply_syn(sk, f, sk->u.tcp.wscale_ok,
                                                    sk->u.tcp.timestamps_ok,
                                                    sk->u.tcp.sack_local_offered,
                                                    sk->u.tcp.ts_recent);
                if (tcp_enqueue_fragment(sk, f) == 0)
                        LOG_INFO("tcp SYN_RECV retransmit SYN+ACK " IP_FMT
                                 ":%u seq=%u ack=%u",
                                 IP_ARG(sk->u.tcp.remote_ip),
                                 rte_be_to_cpu_16(sk->u.tcp.remote_port),
                                 sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);

                sk->u.tcp.retries = 0;
                tcp_arm_syn_timer(sk, TCP_SYN_RTO_MS);
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

        sk->u.tcp.snd_una = sk->u.tcp.sent_seq;
        tcp_sndbuf_reset(&sk->u.tcp.sndbuf, sk->u.tcp.sent_seq);
        tcp_sack_state_reset(&sk->u.tcp, sk->u.tcp.sent_seq);

        /* Handshake done: stop SYN+ACK RTO before ESTABLISHED / accept_queue.
         */
        rte_timer_stop(&sk->u.tcp.timer);
        sk->u.tcp.retries = 0;

        tcp_update_snd_wnd(sk, ntohl(hdr->sent_seq), ntohl(hdr->recv_ack),
                           ntohs(hdr->rx_win),
                           sk->u.tcp.wscale_ok ? sk->u.tcp.snd_wscale : 0);
        tcp_rtt_reset(sk);
        tcp_cc_init_default(&sk->u.tcp, sk->u.tcp.syn_retransmitted);
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
                        LOG_INFO("tcp accept_queue enqueue listener_" TCP_ID_FMT
                                 " peer " IP_FMT ":%u",
                                 TCP_ID_ARG(listener),
                                 IP_ARG(sk->u.tcp.remote_ip),
                                 rte_be_to_cpu_16(sk->u.tcp.remote_port));
                        /* Satisfy one or more owner-parked ACCEPT commands. */
                        socket_owner_wake_accept(listener);
                } else {
                        struct rte_ether_hdr *eth =
                            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);

                        LOG_ERROR(
                            "tcp accept_queue full listener_" TCP_ID_FMT
                            " peer " IP_FMT ":%u; sent RST and free child TCB",
                            TCP_ID_ARG(listener), IP_ARG(sk->u.tcp.remote_ip),
                            rte_be_to_cpu_16(sk->u.tcp.remote_port));

                        tcp_send_reset_for_stream(sk, eth->src_addr.addr_bytes);
                        tcp_drain_send(sk);
                        tcp_drain_recv(sk);
                        nsock_free(sk);
                }
        }
        return 0;
}

/** @brief Process data, ACK, FIN, and out-of-order payload in ESTABLISHED.
 * @param sk Established stream.
 * @param hdr Inbound TCP header.
 * @param mbuf Inbound packet containing the TCP payload.
 * @return 0; payload is copied to TCP-owned storage and ingress frees mbuf.
 */
static int tcp_state_established(struct nsock *sk, struct rte_tcp_hdr *hdr,
                                 struct rte_mbuf *mbuf) {
        uint8_t hdrlen = (hdr->data_off >> 4) * 4;
        uint16_t payload_len =
            rte_be_to_cpu_16(tcp_ipv4_header(mbuf)->total_length) -
            sizeof(struct rte_ipv4_hdr) - hdrlen;

        int has_fin = !!(hdr->tcp_flags & RTE_TCP_FIN_FLAG);

        uint32_t seg_seq = ntohl(hdr->sent_seq);
        uint32_t seg_ack = ntohl(hdr->recv_ack);

        if (hdr->tcp_flags & RTE_TCP_ACK_FLAG) {
                uint8_t scale = sk->u.tcp.wscale_ok
                                    ? sk->u.tcp.snd_wscale
                                    : 0;
                uint32_t advertised = (uint32_t)ntohs(hdr->rx_win) << scale;
                bool classic_dup = payload_len == 0 && !has_fin &&
                                   !(hdr->tcp_flags & RTE_TCP_SYN_FLAG) &&
                                   sk->u.tcp.snd_wnd_valid &&
                                   advertised == sk->u.tcp.snd_wnd;

                tcp_process_peer_ack_ex(sk, seg_ack, classic_dup, true,
                                        seg_seq, ntohs(hdr->rx_win), scale);
        }

        /* ACK and advertised-window state were committed atomically above. */

        if (payload_len == 0 && !has_fin)
                return 0; /* pure ACK: caller frees mbuf */

        /* SYN is unexpected once established; ignore for now. */
        if (hdr->tcp_flags & RTE_TCP_SYN_FLAG)
                return 0;

        uint32_t seq = ntohl(hdr->sent_seq);
        uint32_t rcv_nxt = sk->u.tcp.recv_ack;
        uint32_t wnd_end = rcv_nxt + tcp_rcv_wnd(sk);

        tcp_sack_note_duplicate(sk, seq, payload_len);

        if (!tcp_segment_acceptable(sk, seq, payload_len, has_fin)) {
                if (!tcp_seq_lt(seq, rcv_nxt))
                        tcp_ofo_record_rcv_window_drop(
                            sk, seq, payload_len, has_fin);
                struct tcp_fragment *ack_f =
                    tcp_make_fragment(sk, RTE_TCP_ACK_FLAG, sk->u.tcp.sent_seq,
                                      sk->u.tcp.recv_ack);
                (void)tcp_enqueue_fragment(sk, ack_f);
                return 0; /* caller frees mbuf */
        }

        /*
         * The segment may only partially fall within the receive window; retain
         * only the payload that lies within the window. The FIN flag also
         * consumes a sequence number; if it falls beyond the right edge of the
         * window, it cannot be consumed.
         */
        uint32_t data_end = seq + payload_len;

        if (tcp_seq_gt(data_end, wnd_end)) {
                payload_len = wnd_end - seq;
                has_fin = 0;
        } else if (has_fin && !tcp_seq_lt(data_end, wnd_end)) {
                /** sequence of fin >= right edge of the windows */
                has_fin = 0;
        }

        uint8_t *payload = (uint8_t *)hdr + hdrlen;

        int wake_recv = 0;
        int recv_progress = 0;
        if (seq == rcv_nxt) {
                /*
                 * recv_ack is a delivery boundary, not merely an RX boundary:
                 * if the app queue/allocation is full, retain the segment in
                 * ofo and leave the cumulative ACK unchanged. The peer will
                 * retransmit if no later input lets tcp_ofo_drain retry it.
                 */
                if (payload_len > 0 &&
                    tcp_deliver_payload(sk, payload, payload_len) != 0) {
                        (void)tcp_ofo_insert(sk, seq, payload, payload_len,
                                             has_fin);
                } else {
                        sk->u.tcp.recv_ack += payload_len;
                        sk->u.tcp.rcvbuf_used += payload_len;
                        wake_recv = payload_len > 0;
                        recv_progress = payload_len > 0;
                        if (has_fin) {
                                sk->u.tcp.recv_ack += 1;
                                tcp_stream_set_status(sk,
                                                      TCP_STATUS_CLOSE_WAIT);
                                wake_recv = 1;
                                recv_progress = 1;
                        }
                        if (recv_progress)
                                tcp_options_note_receive_progress(sk);
                        tcp_ofo_drain(sk, tcp_deliver_payload,
                                      tcp_ofo_enter_close_wait);
                }
        } else {
                /* OOO (or partial left-trim happened inside insert). */
                (void)tcp_ofo_insert(sk, seq, payload, payload_len, has_fin);
                /* Do not advance recv_ack; drain is no-op unless trim made it
                 * in-order. */
                tcp_ofo_drain(sk, tcp_deliver_payload,
                              tcp_ofo_enter_close_wait);
        }

        /* Pure ACK so the peer can retire its in-flight bytes. */
        struct tcp_fragment *ack_f = tcp_make_fragment(
            sk, RTE_TCP_ACK_FLAG, sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
        uint16_t ack_win = ack_f == NULL ? 0 : ack_f->rx_win;
        (void)tcp_enqueue_fragment(sk, ack_f);
        LOG_TCP_DEBUG(TCP_SK_FMT " event=ack-queue reason=%s ack=%u win=%u",
                      TCP_SK_ARG(sk),
                      has_fin       ? "data-fin"
                      : payload_len ? "data"
                                    : "ooo",
                      sk->u.tcp.recv_ack, ack_win);
        /*
         * All TCP receive state and the cumulative ACK have been committed.
         * A resumed recv may now safely send a window-update ACK.
         */
        /*
         * OFO drain can deliver bytes even when this input segment carried no
         * directly deliverable payload.  The ready event must reflect either
         * source of newly readable data.
         */
        if (nsock_tcp_rx_count(sk) != 0)
                wake_recv = 1;
        /*
         * An OFO FIN may advance the stream to CLOSE_WAIT without adding a
         * payload blob to recv_buf.  Publish HUP in that case so event-driven
         * consumers observe EOF even when there are no readable bytes.
         */
        if (wake_recv || sk->u.tcp.status == TCP_STATUS_CLOSE_WAIT) {
                socket_owner_wake_recv(sk);
                uint32_t events = OWNER_IO_EV_READ;
                if (sk->u.tcp.status == TCP_STATUS_CLOSE_WAIT)
                        events |= OWNER_IO_EV_HUP;
                socket_owner_ready_post(sk, events);
        }

        return 0;
}

/** @brief Complete passive close after receiving the ACK for the local FIN.
 * @param sk LAST_ACK stream.
 * @param hdr Inbound TCP header.
 * @param mbuf Inbound packet; not retained.
 * @return 0; ingress frees @p mbuf.
 */
static int tcp_state_last_ack(struct nsock *sk, struct rte_tcp_hdr *hdr,
                              struct rte_mbuf *mbuf) {
        // Passive close final step
        (void)mbuf;
        if (sk->u.tcp.status != TCP_STATUS_LAST_ACK)
                return 0;

        /* Peer retransmitted FIN (our ACK-of-FIN was lost): re-ACK. */
        if (hdr->tcp_flags & RTE_TCP_FIN_FLAG) {
                struct tcp_fragment *ack_f =
                    tcp_make_fragment(sk, RTE_TCP_ACK_FLAG, sk->u.tcp.sent_seq,
                                      sk->u.tcp.recv_ack);
                if (tcp_enqueue_fragment(sk, ack_f) == 0) {
                        LOG_TCP_INFO("tcp LAST_ACK re-ACK peer FIN " IP_FMT
                                     ":%u ack=%u",
                                     IP_ARG(sk->u.tcp.remote_ip),
                                     rte_be_to_cpu_16(sk->u.tcp.remote_port),
                                     sk->u.tcp.recv_ack);
                }
        }

        if (!(hdr->tcp_flags & RTE_TCP_ACK_FLAG))
                return 0;

        uint32_t acknum = ntohl(hdr->recv_ack);
        /* Retire any still-unacked data (and FIN seq) before the final check.
         */
        tcp_process_peer_ack(sk, acknum);

        if (acknum != sk->u.tcp.sent_seq) {
                LOG_DEBUG(
                    "tcp LAST_ACK ignore ack=%u expect=%u from " IP_FMT ":%u",
                    acknum, sk->u.tcp.sent_seq, IP_ARG(sk->u.tcp.remote_ip),
                    rte_be_to_cpu_16(sk->u.tcp.remote_port));
                return 0;
        }

        tcp_stream_set_status(sk, TCP_STATUS_CLOSED);
        tcp_drain_send(sk);
        tcp_drain_recv(sk);
        /*
         * nclose already detached the fd and returned after starting
         * LAST_ACK, so terminal protocol processing owns final reclamation.
         */
        if (sk->app_closed)
                nsock_free(sk);
        return 0;
}

/** @brief Re-ACK a retransmitted peer FIN while in TIME_WAIT.
 * @param sk TIME_WAIT stream.
 * @param hdr Inbound TCP header.
 * @param mbuf Inbound packet; not retained.
 * @return 0; ingress frees @p mbuf.
 */
static int tcp_state_time_wait(struct nsock *sk, struct rte_tcp_hdr *hdr,
                               struct rte_mbuf *mbuf) {
        (void)mbuf;
        if (sk->u.tcp.status != TCP_STATUS_TIME_WAIT)
                return 0;

        /* Peer retransmitted FIN (our ACK-of-FIN was lost): re-ACK. */
        if (hdr->tcp_flags & RTE_TCP_FIN_FLAG) {
                struct tcp_fragment *ack_f =
                    tcp_make_fragment(sk, RTE_TCP_ACK_FLAG, sk->u.tcp.sent_seq,
                                      sk->u.tcp.recv_ack);
                if (tcp_enqueue_fragment(sk, ack_f) == 0) {
                        LOG_TCP_INFO("tcp TIME_WAIT re-ACK peer FIN " IP_FMT
                                     ":%u ack=%u",
                                     IP_ARG(sk->u.tcp.remote_ip),
                                     rte_be_to_cpu_16(sk->u.tcp.remote_port),
                                     sk->u.tcp.recv_ack);
                }
        }
        return 0;
}

/** @brief Finish simultaneous close after the peer acknowledges the local FIN.
 * @param sk CLOSING stream.
 * @param hdr Inbound TCP header.
 * @param mbuf Inbound packet; not retained.
 * @return 0; ingress frees @p mbuf.
 */
static int tcp_state_closing(struct nsock *sk, struct rte_tcp_hdr *hdr,
                             struct rte_mbuf *mbuf) {

        (void)mbuf;
        if (sk->u.tcp.status != TCP_STATUS_CLOSING) {
                return 0;
        }

        /* Peer retransmitted FIN (our ACK-of-FIN was lost): re-ACK. */
        if (hdr->tcp_flags & RTE_TCP_FIN_FLAG) {
                struct tcp_fragment *ack_f =
                    tcp_make_fragment(sk, RTE_TCP_ACK_FLAG, sk->u.tcp.sent_seq,
                                      sk->u.tcp.recv_ack);
                if (tcp_enqueue_fragment(sk, ack_f) == 0) {
                        LOG_TCP_INFO("tcp CLOSING re-ACK peer FIN " IP_FMT
                                     ":%u ack=%u",
                                     IP_ARG(sk->u.tcp.remote_ip),
                                     rte_be_to_cpu_16(sk->u.tcp.remote_port),
                                     sk->u.tcp.recv_ack);
                }
        }

        if (!(hdr->tcp_flags & RTE_TCP_ACK_FLAG))
                return 0;

        uint32_t acknum = ntohl(hdr->recv_ack);
        tcp_process_peer_ack(sk, acknum);

        if (acknum != sk->u.tcp.sent_seq) {
                LOG_DEBUG(
                    "tcp CLOSING ignore ack=%u expect=%u from " IP_FMT ":%u",
                    acknum, sk->u.tcp.sent_seq, IP_ARG(sk->u.tcp.remote_ip),
                    rte_be_to_cpu_16(sk->u.tcp.remote_port));
                return 0;
        }

        LOG_TCP_INFO("tcp CLOSING rx ACK -> TIME_WAIT " TCP_ID_FMT,
                     TCP_ID_ARG(sk));
        tcp_enter_time_wait(sk);
        return 0;
}

/** @brief Process ACK and FIN combinations while waiting for local FIN ACK.
 * @param sk FIN_WAIT_1 stream.
 * @param hdr Inbound TCP header.
 * @param mbuf Inbound packet used for diagnostic payload length.
 * @return 0; ingress frees @p mbuf.
 */
static int tcp_state_fin_wait_1(struct nsock *sk, struct rte_tcp_hdr *hdr,
                                struct rte_mbuf *mbuf) {
        int has_fin = !!(hdr->tcp_flags & RTE_TCP_FIN_FLAG);
        int has_ack = !!(hdr->tcp_flags & RTE_TCP_ACK_FLAG);
        uint32_t acknum = has_ack ? ntohl(hdr->recv_ack) : 0;

        /* Retire unacked sndbuf data (and FIN) before state transitions. */
        if (has_ack)
                tcp_process_peer_ack(sk, acknum);

        int ack_ok = has_ack && (acknum == sk->u.tcp.sent_seq);
        uint32_t seq = ntohl(hdr->sent_seq);

        if (seq != sk->u.tcp.recv_ack) {
                LOG_DEBUG("tcp FIN_WAIT_1 drop ooo/dup seq=%u expect=%u", seq,
                          sk->u.tcp.recv_ack);
                return 0;
        }

        if (has_fin) {
                sk->u.tcp.recv_ack = seq + 1;
                struct tcp_fragment *ack_f =
                    tcp_make_fragment(sk, RTE_TCP_ACK_FLAG, sk->u.tcp.sent_seq,
                                      sk->u.tcp.recv_ack);
                if (tcp_enqueue_fragment(sk, ack_f) == 0) {
                        LOG_TCP_INFO("tcp FIN_WAIT_1 ACK peer FIN " IP_FMT
                                     ":%u ack=%u",
                                     IP_ARG(sk->u.tcp.remote_ip),
                                     rte_be_to_cpu_16(sk->u.tcp.remote_port),
                                     sk->u.tcp.recv_ack);
                }

                if (ack_ok) {
                        tcp_enter_time_wait(sk);
                } else {
                        tcp_stream_set_status(sk, TCP_STATUS_CLOSING);
                }
                return 0;
        }

        if (ack_ok) {
                tcp_stream_set_status(sk, TCP_STATUS_FIN_WAIT_2);
                return 0;
        }

        LOG_DEBUG(
            "tcp FIN_WAIT_1 drop seq=%u expect=%u len=%u flags=%s", seq,
            sk->u.tcp.recv_ack,
            (unsigned)(rte_be_to_cpu_16(tcp_ipv4_header(mbuf)->total_length) -
                       sizeof(struct rte_ipv4_hdr) - (hdr->data_off >> 4) * 4),
            tcp_flags_str(hdr->tcp_flags));
        return 0; /* caller frees mbuf */
}

/** @brief Process the peer FIN after the local FIN has been acknowledged.
 * @param sk FIN_WAIT_2 stream.
 * @param hdr Inbound TCP header.
 * @param mbuf Inbound packet; not retained.
 * @return 0; ingress frees @p mbuf.
 */
static int tcp_state_fin_wait_2(struct nsock *sk, struct rte_tcp_hdr *hdr,
                                struct rte_mbuf *mbuf) {
        if (sk->u.tcp.status != TCP_STATUS_FIN_WAIT_2)
                return 0;

        /* Harmless if already fully ACKed; covers late/dup data ACKs. */
        if (hdr->tcp_flags & RTE_TCP_ACK_FLAG)
                tcp_process_peer_ack(sk, ntohl(hdr->recv_ack));

        /* Only peer FIN advances us. */
        if (!(hdr->tcp_flags & RTE_TCP_FIN_FLAG))
                return 0;

        uint32_t seq = ntohl(hdr->sent_seq);
        if (seq != sk->u.tcp.recv_ack) {
                LOG_DEBUG("tcp FIN_WAIT_2 drop ooo/dup seq=%u expect=%u", seq,
                          sk->u.tcp.recv_ack);
                return 0;
        }

        uint8_t hdrlen = (hdr->data_off >> 4) * 4;
        uint16_t ip_len = rte_be_to_cpu_16(tcp_ipv4_header(mbuf)->total_length);
        uint16_t payload_len = ip_len - sizeof(struct rte_ipv4_hdr) - hdrlen;
        uint8_t *payload = (uint8_t *)hdr + hdrlen;

        /*
         * The FIN sequence number immediately follows the payload. If delivery
         * fails, do not acknowledge the payload or the FIN; keep recv_ack
         * unchanged to prompt the peer to retransmit the entire segment based
         * on RTO.
         */
        if (payload_len > 0) {
                if (tcp_deliver_payload(sk, payload, payload_len) != 0) {
                        struct tcp_fragment *ack_f = tcp_make_fragment(
                            sk, RTE_TCP_ACK_FLAG, sk->u.tcp.sent_seq,
                            sk->u.tcp.recv_ack);
                        (void)tcp_enqueue_fragment(sk, ack_f);
                        return 0;
                }
                sk->u.tcp.recv_ack += payload_len;
                sk->u.tcp.rcvbuf_used += payload_len;
        }

        /* Payload has been delivered; now acknowledge the FIN. */
        sk->u.tcp.recv_ack += 1;

        struct tcp_fragment *ack_f = tcp_make_fragment(
            sk, RTE_TCP_ACK_FLAG, sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
        if (tcp_enqueue_fragment(sk, ack_f) == 0) {
                LOG_INFO("tcp FIN_WAIT_2 ACK peer FIN " IP_FMT ":%u ack=%u",
                         IP_ARG(sk->u.tcp.remote_ip),
                         rte_be_to_cpu_16(sk->u.tcp.remote_port),
                         sk->u.tcp.recv_ack);
        }
        LOG_INFO("tcp FIN_WAIT_2 rx FIN -> TIME_WAIT " IP_FMT ":%u ack=%u",
                 IP_ARG(sk->u.tcp.remote_ip),
                 rte_be_to_cpu_16(sk->u.tcp.remote_port), sk->u.tcp.recv_ack);
        tcp_enter_time_wait(sk);

        return 0;
}

/**
 * @brief Re-ACK a retransmitted peer FIN while the application is in
 * CLOSE_WAIT.
 *
 * Peer retransmitted FIN while we wait for the app's nclose (CLOSE_WAIT).
 * Re-ACK so the peer can retire its FIN if our first ACK-of-FIN was lost.
 * Our own FIN is sent later from tcp_close().
 * @param sk CLOSE_WAIT stream.
 * @param hdr Inbound TCP header.
 * @param mbuf Inbound packet; not retained.
 * @return 0; ingress frees @p mbuf.
 */
static int tcp_state_close_wait(struct nsock *sk, struct rte_tcp_hdr *hdr,
                                struct rte_mbuf *mbuf) {
        (void)mbuf;
        if (sk->u.tcp.status != TCP_STATUS_CLOSE_WAIT)
                return 0;

        /* App may still have unacked data sent before peer FIN. */
        if (hdr->tcp_flags & RTE_TCP_ACK_FLAG)
                tcp_process_peer_ack(sk, ntohl(hdr->recv_ack));

        if (hdr->tcp_flags & RTE_TCP_FIN_FLAG) {
                struct tcp_fragment *ack_f =
                    tcp_make_fragment(sk, RTE_TCP_ACK_FLAG, sk->u.tcp.sent_seq,
                                      sk->u.tcp.recv_ack);
                if (tcp_enqueue_fragment(sk, ack_f) == 0) {
                        LOG_INFO("tcp CLOSE_WAIT re-ACK peer FIN " IP_FMT
                                 ":%u ack=%u",
                                 IP_ARG(sk->u.tcp.remote_ip),
                                 rte_be_to_cpu_16(sk->u.tcp.remote_port),
                                 sk->u.tcp.recv_ack);
                }
        }
        return 0;
}

/**
 * @brief Drop a segment received in a state without a dedicated handler.
 *
 * Closed sockets are treated as unmatched by tcp_ingress, which generates an
 * RST where appropriate. LISTEN processing is also performed by tcp_ingress.
 * @param sk Socket selected by ingress.
 * @param hdr Inbound TCP header.
 * @param mbuf Inbound packet; not retained.
 * @return 0; ingress frees @p mbuf.
 */
static int tcp_state_drop(struct nsock *sk, struct rte_tcp_hdr *hdr,
                          struct rte_mbuf *mbuf) {
        (void)sk;
        (void)hdr;
        (void)mbuf;
        return 0;
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
    [TCP_STATUS_SYN_SENT] = {tcp_state_syn_sent},
    [TCP_STATUS_SYN_RECV] = {tcp_state_syn_recv},
    [TCP_STATUS_ESTABLISHED] = {tcp_state_established},
    [TCP_STATUS_CLOSE_WAIT] = {tcp_state_close_wait},
    [TCP_STATUS_LAST_ACK] = {tcp_state_last_ack},
    [TCP_STATUS_TIME_WAIT] = {tcp_state_time_wait},
    [TCP_STATUS_CLOSING] = {tcp_state_closing},
    [TCP_STATUS_FIN_WAIT_1] = {tcp_state_fin_wait_1},
    [TCP_STATUS_FIN_WAIT_2] = {tcp_state_fin_wait_2},
};

/**
 * @brief Parse and dispatch one inbound IPv4/TCP packet.
 * @param mbuf Packet received from the worker input ring.
 * @return Always 0; this function always consumes @p mbuf.
 *
 * Learns the peer MAC, selects an existing stream or listener, and delegates
 * state handling. A handler may retain the mbuf only by returning non-zero.
 */
int tcp_ingress(struct rte_mbuf *mbuf) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *iphdr = tcp_ipv4_header(mbuf);
        struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(iphdr + 1);
        const uint16_t l2_l3_len =
            sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);
        const uint16_t l4_len =
            rte_be_to_cpu_16(iphdr->total_length) - sizeof(*iphdr);

        /*
         * IPv4 length and header checksum were validated by dispatch_packet().
         * Validate the TCP header before reading its fields, then verify the
         * TCP checksum over the pseudo-header and complete (possibly
         * multi-segment) L4 payload.  Do this before ARP learning, RST
         * generation, or state transitions so corrupt traffic has no effects.
         */
        if (mbuf->data_len < l2_l3_len + sizeof(*tcp_hdr) ||
            l4_len < sizeof(*tcp_hdr)) {
                LOG_DEBUG("dropping invalid TCP segment");
                rte_pktmbuf_free(mbuf);
                return 0;
        }

        const uint8_t tcp_hdr_len = (tcp_hdr->data_off >> 4) * 4;
        if (tcp_hdr_len < sizeof(*tcp_hdr) || tcp_hdr_len > l4_len ||
            mbuf->data_len < l2_l3_len + tcp_hdr_len ||
            rte_ipv4_udptcp_cksum_mbuf_verify(mbuf, iphdr, l2_l3_len) != 0) {
                LOG_DEBUG("dropping invalid TCP segment");
                rte_pktmbuf_free(mbuf);
                return 0;
        }

        LOG_TCP_PACKET(
            "event=rx " IP_FMT ":%u->" IP_FMT
            ":%u flags=%s seq=%u ack=%u win=%u",
            IP_ARG(iphdr->src_addr), rte_be_to_cpu_16(tcp_hdr->src_port),
            IP_ARG(iphdr->dst_addr), rte_be_to_cpu_16(tcp_hdr->dst_port),
            tcp_flags_str(tcp_hdr->tcp_flags), ntohl(tcp_hdr->sent_seq),
            ntohl(tcp_hdr->recv_ack), ntohs(tcp_hdr->rx_win));

        arp_table_confirm(iphdr->src_addr, eth->src_addr.addr_bytes);

        struct tcp_options_rx opts;
        if (tcp_options_parse(tcp_hdr, &opts) != 0) {
                LOG_ERROR("dropping malformed TCP options");
                rte_pktmbuf_free(mbuf);
                return 0;
        }

        uint32_t remote_ip = iphdr->src_addr;
        uint32_t local_ip = iphdr->dst_addr;
        uint16_t remote_port = tcp_hdr->src_port;
        uint16_t local_port = tcp_hdr->dst_port;

        struct nsock *sk =
            tcp_stream_search(remote_ip, local_ip, remote_port, local_port);

        /*
         * A locally retained CLOSED socket must not suppress RFC 793's
         * unmatched-segment RST response.
         */
        if (sk != NULL && sk->u.tcp.status == TCP_STATUS_CLOSED)
                sk = NULL;

        /* Existing TCB (including SYN_RECV children still awaiting final ACK).
         */
        if (sk != NULL) {
                if (tcp_hdr->tcp_flags & RTE_TCP_RST_FLAG) {
                        if (tcp_rst_acceptable(sk, tcp_hdr))
                                tcp_abort_on_rst(sk);
                        else
                                LOG_WARN(
                                    "tcp ignored unacceptable RST " TCP_ID_FMT
                                    " "
                                    "state=%s seq=%u ack=%u",
                                    TCP_ID_ARG(sk),
                                    tcp_status_str(sk->u.tcp.status),
                                    ntohl(tcp_hdr->sent_seq),
                                    ntohl(tcp_hdr->recv_ack));

                        rte_pktmbuf_free(mbuf);
                        return 0;
                }
                TCP_STATUS st = sk->u.tcp.status;
                if (st == TCP_STATUS_SYN_SENT &&
                    (tcp_hdr->tcp_flags &
                     (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG)) ==
                        (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG) &&
                    ntohl(tcp_hdr->recv_ack) == sk->u.tcp.sent_seq + 1) {
                        tcp_options_negotiate_syn(sk, &opts);
                }
                uint32_t seg_seq = ntohl(tcp_hdr->sent_seq);
                uint16_t seg_len = l4_len - tcp_hdr_len;
                bool has_fin = !!(tcp_hdr->tcp_flags & RTE_TCP_FIN_FLAG);
                /*
                 * ESTABLISHED is the state with a complete RFC 793 receive
                 * window rule.  Its result is shared with PAWS so a stale,
                 * otherwise acceptable segment is discarded before the state
                 * handler can update ACK/window/RTT state.
                 */
                bool seq_acceptable =
                    st == TCP_STATUS_ESTABLISHED &&
                    tcp_segment_acceptable(sk, seg_seq, seg_len, has_fin);
                int ts_result = tcp_options_process_inbound(
                    sk, &opts, false, seg_seq, seq_acceptable);
                if (ts_result != 0) {
                        if (ts_result == -2 &&
                            st == TCP_STATUS_ESTABLISHED) {
                                /* PAWS still requires an ACK. RFC 2883 allows
                                 * that ACK to identify duplicate payload. */
                                tcp_sack_note_duplicate(sk, seg_seq, seg_len);
                                struct tcp_fragment *ack_f = tcp_make_fragment(
                                    sk, RTE_TCP_ACK_FLAG,
                                    sk->u.tcp.sent_seq,
                                    sk->u.tcp.recv_ack);
                                (void)tcp_enqueue_fragment(sk, ack_f);
                        }
                        LOG_DEBUG(ts_result == -2
                                      ? "dropping stale Timestamp (PAWS)"
                                      : "dropping segment without negotiated "
                                        "Timestamp option");
                        rte_pktmbuf_free(mbuf);
                        return 0;
                }
                int delivered = 0;
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
                        (void)tcp_state_listen(listener, tcp_hdr, eth, iphdr,
                                               remote_ip, remote_port, &opts);
                } else {
                        tcp_send_reset_reply(eth, iphdr, tcp_hdr);
                        LOG_DEBUG(
                            "tcp SYN to non-listening " IP_FMT
                            ":%u from " IP_FMT ":%u; sent RST",
                            IP_ARG(local_ip), rte_be_to_cpu_16(local_port),
                            IP_ARG(remote_ip), rte_be_to_cpu_16(remote_port));
                }
                rte_pktmbuf_free(mbuf);
                return 0;
        }

        if (!(tcp_hdr->tcp_flags & RTE_TCP_RST_FLAG))
                tcp_send_reset_reply(eth, iphdr, tcp_hdr);

        LOG_DEBUG("tcp unmatched " IP_FMT ":%u -> " IP_FMT ":%u flags=%s",
                  IP_ARG(remote_ip), rte_be_to_cpu_16(remote_port),
                  IP_ARG(local_ip), rte_be_to_cpu_16(local_port),
                  tcp_flags_str(tcp_hdr->tcp_flags));
        rte_pktmbuf_free(mbuf);
        return 0;
}

/** @brief Build an Ethernet/IPv4/TCP packet from a fragment descriptor.
 * @param mp Mempool from which to allocate the packet mbuf.
 * @param src_ip Source IPv4 address in network byte order.
 * @param dst_ip Destination IPv4 address in network byte order.
 * @param dst_mac Destination Ethernet address.
 * @param f Fragment describing TCP header fields and optional payload.
 * @return Newly built packet mbuf, or NULL on allocation failure.
 */
static struct rte_mbuf *tcp_build_pkt(struct rte_mempool *mp, uint32_t src_ip,
                                      uint32_t dst_ip, const uint8_t *dst_mac,
                                      const struct tcp_fragment *f) {
        if (f->opt_len < 0 || f->opt_len > TCP_MAX_OPTIONS ||
            f->data_off != (uint8_t)((5 + f->opt_len) << 4)) {
                LOG_ERROR("invalid TCP option descriptor");
                return NULL;
        }

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

        if (opt_bytes > 0) {
                rte_memcpy((uint8_t *)tcp + sizeof(struct rte_tcp_hdr),
                           f->options, opt_bytes);
        }

        if (f->payload_len > 0)
                rte_memcpy((uint8_t *)tcp + sizeof(struct rte_tcp_hdr) +
                               opt_bytes,
                           f->payload, f->payload_len);

        tcp->cksum = rte_ipv4_udptcp_cksum(ip, tcp);
        return mbuf;
}

static void tcp_emit_fragment(uint32_t src_ip, uint32_t dst_ip,
                              const uint8_t *dst_mac,
                              const struct tcp_fragment *f) {
        if (g_net.mp == NULL) {
                LOG_ERROR("tcp RST: global mbuf pool is unavailable");
                return;
        }

        /*
         * tcp_build_pkt does not retain f or dst_mac. The caller may therefore
         * pass stack storage and the source-MAC bytes from the received frame.
         */
        struct rte_mbuf *out =
            tcp_build_pkt(g_net.mp, src_ip, dst_ip, dst_mac, f);
        if (out == NULL) {
                return;
        }

        struct inout_ring *ring = ring_instance();
        if (rte_ring_sp_enqueue(ring->out, out) != 0) {
                LOG_ERROR("tcp RST: NIC output ring full");
                rte_pktmbuf_free(out);
        }
}

/*
 * RFC 793 reset generation for a segment that does not match any TCB:
 *
 *   incoming ACK:      <SEQ=SEG.ACK><CTL=RST>
 *   incoming non-ACK:  <SEQ=0><ACK=SEG.SEQ+SEG.LEN><CTL=RST,ACK>
 *
 * SEG.LEN includes payload bytes and SYN/FIN sequence-space consumption.
 */
static void tcp_send_reset_reply(const struct rte_ether_hdr *eth,
                                 const struct rte_ipv4_hdr *iphdr,
                                 const struct rte_tcp_hdr *tcp_hdr) {
        if (tcp_hdr->tcp_flags & RTE_TCP_RST_FLAG)
                return; /* Never answer a reset with another reset. */

        uint8_t tcp_hdr_len = (tcp_hdr->data_off >> 4) * 4;
        uint16_t ip_len = rte_be_to_cpu_16(iphdr->total_length);
        if (tcp_hdr_len < sizeof(*tcp_hdr) ||
            ip_len < sizeof(*iphdr) + tcp_hdr_len) {
                LOG_WARN(
                    "tcp RST: malformed segment, cannot calculate SEG.LEN");
                return;
        }

        uint32_t seg_len = ip_len - sizeof(*iphdr) - tcp_hdr_len;
        if (tcp_hdr->tcp_flags & RTE_TCP_SYN_FLAG)
                seg_len++;
        if (tcp_hdr->tcp_flags & RTE_TCP_FIN_FLAG)
                seg_len++;

        struct tcp_fragment rst;
        memset(&rst, 0, sizeof(rst));
        rst.src_port = tcp_hdr->dst_port;
        rst.dst_port = tcp_hdr->src_port;
        rst.data_off = (5 << 4);

        if (tcp_hdr->tcp_flags & RTE_TCP_ACK_FLAG) {
                rst.sent_seq = ntohl(tcp_hdr->recv_ack);
                rst.tcp_flags = RTE_TCP_RST_FLAG;
        } else {
                rst.recv_ack = ntohl(tcp_hdr->sent_seq) + seg_len;
                rst.tcp_flags = RTE_TCP_RST_FLAG | RTE_TCP_ACK_FLAG;
        }

        tcp_emit_fragment(iphdr->dst_addr, iphdr->src_addr,
                          eth->src_addr.addr_bytes, &rst);

        LOG_TCP_INFO("event=tx-unmatched-rst " IP_FMT ":%u->" IP_FMT
                     ":%u flags=%s seq=%u ack=%u",
                     IP_ARG(iphdr->dst_addr), rte_be_to_cpu_16(rst.src_port),
                     IP_ARG(iphdr->src_addr), rte_be_to_cpu_16(rst.dst_port),
                     tcp_flags_str(rst.tcp_flags), rst.sent_seq, rst.recv_ack);
}

/* Send an abortive reset for an existing stream. */
static void tcp_send_reset_for_stream(const struct nsock *sk,
                                      const uint8_t *dst_mac) {
        struct tcp_fragment rst;
        memset(&rst, 0, sizeof(rst));
        rst.src_port = sk->local_port;
        rst.dst_port = sk->u.tcp.remote_port;
        rst.sent_seq = sk->u.tcp.sent_seq;
        rst.recv_ack = sk->u.tcp.recv_ack;
        rst.data_off = (5 << 4);
        rst.tcp_flags = RTE_TCP_RST_FLAG | RTE_TCP_ACK_FLAG;

        tcp_emit_fragment(sk->local_ip, sk->u.tcp.remote_ip, dst_mac, &rst);

        LOG_TCP_INFO(TCP_SK_FMT " event=tx-abort-rst seq=%u ack=%u",
                     TCP_SK_ARG(sk), rst.sent_seq, rst.recv_ack);
}

/*
 * SYN_SENT validates RST by ACKing our SYN.
 * Synchronized states (SYN_RECV, ESTABLISHED, etc.) use an exact
 * RCV.NXT match.
 * Exact matching is stricter than window acceptance, but is
 * appropriate for this stack and avoids accepting stale/spoofed resets.
 */
static int tcp_rst_acceptable(const struct nsock *sk,
                              const struct rte_tcp_hdr *hdr) {
        if (!(hdr->tcp_flags & RTE_TCP_RST_FLAG))
                return 0;

        if (sk->u.tcp.status == TCP_STATUS_SYN_SENT) {
                return (hdr->tcp_flags & RTE_TCP_ACK_FLAG) &&
                       ntohl(hdr->recv_ack) == sk->u.tcp.sent_seq + 1;
        }

        if (sk->u.tcp.status == TCP_STATUS_TIME_WAIT)
                return 0; /* RFC 793: ignore RST in TIME_WAIT. */

        return ntohl(hdr->sent_seq) == sk->u.tcp.recv_ack;
}

/*
 * Do not free an ESTABLISHED, not-yet-visible child on RST: it may already be
 * stored in the listener's accept_queue. tcp_accept_owned() removes the queue
 * reference and then reclaims that CLOSED child.
 */
static void tcp_abort_on_rst(struct nsock *sk) {
        TCP_STATUS old = sk->u.tcp.status;

        LOG_WARN("tcp accepted RST " TCP_ID_FMT " state=%s peer " IP_FMT ":%u",
                 TCP_ID_ARG(sk), tcp_status_str(old),
                 IP_ARG(sk->u.tcp.remote_ip),
                 rte_be_to_cpu_16(sk->u.tcp.remote_port));
        rte_timer_stop(&sk->u.tcp.timer);

        if (old == TCP_STATUS_SYN_RECV) {
                struct nsock *listener = sk->u.tcp.listener;
                if (listener != NULL && listener->u.tcp.syn_pending > 0) {
                        listener->u.tcp.syn_pending--;
                }

                tcp_drain_send(sk);
                tcp_drain_recv(sk);
                nsock_free(sk);
                return;
        }

        tcp_stream_set_status(sk, TCP_STATUS_CLOSED);
        tcp_drain_send(sk);
        tcp_drain_recv(sk);

        /* RST completes every command parked on this owner-side socket. */
        socket_owner_abort_waiters(sk, ECONNRESET);
        socket_owner_ready_post(sk, OWNER_IO_EV_ERROR | OWNER_IO_EV_HUP);
        if (sk->app_closed)
                nsock_free(sk);
}

/**
 * @brief Transmit one MSS-limited unsent range from a stream send buffer.
 * @param sk Stream whose sndbuf is flushed.
 * @param mp Mempool used to create IPv4/TCP packets and ARP requests.
 * @return 1 when a data segment was queued for NIC output, otherwise 0.
 *
 * Buffered bytes remain in sndbuf until acknowledged; this helper advances
 * sent_seq and arms the data RTO only after successful packet construction.
 * Packet construction copies directly from @c sndbuf. Both this helper and
 * @ref tcp_send execute on the socket's owner lcore, so the source pointer
 * remains stable until the copy completes.
 */
enum tcp_sndbuf_flush_result {
        TCP_SNDBUF_IDLE = 0,
        TCP_SNDBUF_SENT,
        TCP_SNDBUF_RETRY,
        TCP_SNDBUF_ARP_WAIT,
};

/**
 * @brief Transmit one recovery-selected range without changing SND.NXT.
 * @param sk Stream with a retained sndbuf and optional pending candidate.
 * @param mp Mempool used to build the retransmission packet.
 * @return Sent, retry, ARP-wait, or idle scheduling result.
 *
 * RFC 6675 schedules candidates using cwnd - Pipe; NewReno partial ACK and RTO
 * recovery install an explicit lowest-hole candidate.  Every mode reads the
 * chosen sequence directly from ACK-retained sndbuf. HighRxt, Pipe, rescue
 * state, and Karn suppression are committed only after successful TX-ring
 * enqueue, so transient failures preserve the same candidate for retry.
 */
static enum tcp_sndbuf_flush_result
tcp_tx_flush_recovery_retransmit(struct nsock *sk, struct rte_mempool *mp) {
        enum tcp_sndbuf_flush_result result = TCP_SNDBUF_IDLE;
        uint32_t arp_wait_ip = 0;

        if (sk->u.tcp.sack.pending.kind == TCP_RECOVERY_TX_NONE &&
            sk->u.tcp.sack.mode == TCP_RECOVERY_SACK) {
                uint32_t flight_end =
                    tcp_sack_payload_flight_end(sk);
                uint32_t pipe = tcp_sack_set_pipe(&sk->u.tcp, flight_end);
                uint32_t cwnd = tcp_cc_send_window(&sk->u.tcp);
                uint32_t smss = sk->u.tcp.snd_mss;

                if (cwnd >= pipe && cwnd - pipe >= smss) {
                        uint32_t sndbuf_end = sk->u.tcp.sndbuf.head_seq +
                                              sk->u.tcp.sndbuf.len;
                        (void)tcp_sack_schedule_next(
                            &sk->u.tcp, sndbuf_end,
                            tcp_seq_lt(sk->u.tcp.sent_seq, sndbuf_end));
                }
        }
        if (sk->u.tcp.sack.pending.kind != TCP_RECOVERY_TX_RETRANSMIT)
                goto out;

        struct tcp_sndbuf *sb = &sk->u.tcp.sndbuf;
        uint32_t seq = sk->u.tcp.sack.pending.seq;
        uint32_t end = sk->u.tcp.sack.pending.end;
        uint32_t flight_end = tcp_sack_payload_flight_end(sk);
        if (sb->len == 0 || tcp_seq_lt(seq, sb->head_seq) ||
            !tcp_seq_lt(seq, end) || tcp_seq_gt(end, flight_end)) {
                tcp_sack_cancel_candidate(&sk->u.tcp);
                goto out;
        }

        uint32_t chunk_available = 0;
        const uint8_t *payload = tcp_sndbuf_peek(sb, seq, &chunk_available);
        if (payload == NULL) {
                tcp_sack_cancel_candidate(&sk->u.tcp);
                goto out;
        }

        struct inout_ring *ring = ring_instance();
        const uint8_t *dst_mac = arp_resolve(mp, ring->out, sk->u.tcp.remote_ip,
                                             rte_get_timer_cycles());
        if (dst_mac == NULL) {
                arp_wait_ip = sk->u.tcp.remote_ip;
                result = TCP_SNDBUF_ARP_WAIT;
                goto out;
        }

        struct tcp_fragment f;
        memset(&f, 0, sizeof(f));
        f.src_port = sk->local_port;
        f.dst_port = sk->u.tcp.remote_port;
        f.sent_seq = seq;
        f.recv_ack = sk->u.tcp.recv_ack;
        f.tcp_flags = RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG;
        f.data_off = (5 << 4);
        f.rx_win = tcp_wire_rcv_wnd(sk, f.tcp_flags);
        if (tcp_options_apply_established(sk, &f) != 0) {
                result = TCP_SNDBUF_RETRY;
                goto out;
        }

        uint32_t seglen = end - seq;
        uint32_t mss = tcp_options_data_mss(sk, &f);
        if (seglen > mss)
                seglen = mss;
        if (seglen > chunk_available)
                seglen = chunk_available;
        if (seglen == 0) {
                tcp_sack_cancel_candidate(&sk->u.tcp);
                goto out;
        }
        f.payload = (unsigned char *)payload;
        f.payload_len = seglen;

        struct rte_mbuf *tcp_buf =
            tcp_build_pkt(mp, g_net.local_ip, sk->u.tcp.remote_ip, dst_mac, &f);
        if (tcp_buf == NULL) {
                result = TCP_SNDBUF_RETRY;
                goto out;
        }
        if (rte_ring_sp_enqueue(ring->out, tcp_buf) != 0) {
                rte_pktmbuf_free(tcp_buf);
                result = TCP_SNDBUF_RETRY;
                goto out;
        }

        tcp_sack_commit_candidate(&sk->u.tcp, seq + seglen);
        tcp_rtt_on_retransmit(sk);
        struct tcp_cc_tx_event cc_tx = {
            .bytes = seglen,
            .now_ms = tcp_options_now_ms(),
            .retransmission = true,
            .cwnd_limited = sk->u.tcp.cc.cwnd_limited,
        };
        tcp_cc_on_packet_sent(&sk->u.tcp, &cc_tx);
        LOG_TCP_PACKET(TCP_SK_FMT
                       " event=tx-recovery-retransmit seq=%u ack=%u len=%u "
                       "snd_nxt=%u",
                       TCP_SK_ARG(sk), seq, f.recv_ack, seglen,
                       sk->u.tcp.sent_seq);
        result = TCP_SNDBUF_SENT;

out:
        if (result == TCP_SNDBUF_ARP_WAIT)
                nsock_tx_arp_wait(sk, arp_wait_ip);
        return result;
}

static enum tcp_sndbuf_flush_result
tcp_tx_flush_sndbuf(struct nsock *sk, struct rte_mempool *mp) {
        enum tcp_sndbuf_flush_result result = TCP_SNDBUF_IDLE;
        uint32_t arp_wait_ip = 0;

        /* tcp_send() and this flush are serialized by the socket owner. */

        if (sk->u.tcp.status != TCP_STATUS_ESTABLISHED &&
            sk->u.tcp.status != TCP_STATUS_CLOSE_WAIT &&
            sk->u.tcp.status != TCP_STATUS_FIN_WAIT_1 &&
            sk->u.tcp.status != TCP_STATUS_CLOSING &&
            sk->u.tcp.status != TCP_STATUS_LAST_ACK)
                goto out;

        struct tcp_sndbuf *sb = &sk->u.tcp.sndbuf;
        if (sb->head == NULL || sb->len == 0)
                goto out;

        /* Bytes not yet transmitted: [sent_seq, head_seq + len) */
        uint32_t buf_end = sb->head_seq + sb->len;
        if (!tcp_seq_lt(sk->u.tcp.sent_seq, buf_end))
                goto out;

        uint32_t unsent = buf_end - sk->u.tcp.sent_seq;
        uint32_t chunk_available = 0;
        const uint8_t *payload =
            tcp_sndbuf_peek(sb, sk->u.tcp.sent_seq, &chunk_available);
        if (payload == NULL)
                goto out;

        uint32_t in_flight = sk->u.tcp.sent_seq - sk->u.tcp.snd_una;
        int was_idle = (sk->u.tcp.snd_una == sk->u.tcp.sent_seq);
        uint32_t congestion_in_flight = in_flight;
        if (sk->u.tcp.sack.mode == TCP_RECOVERY_SACK ||
            sk->u.tcp.sack.limited_transmit) {
                uint32_t flight_end =
                    tcp_sack_payload_flight_end(sk);
                congestion_in_flight =
                    tcp_sack_set_pipe(&sk->u.tcp, flight_end);
        }

        uint32_t send_window = sk->u.tcp.snd_wnd;
        uint32_t cwnd = tcp_cc_send_window(&sk->u.tcp);
        if (cwnd < send_window)
                send_window = cwnd;
        if (congestion_in_flight >= send_window)
                goto out;

        uint32_t credit = send_window - congestion_in_flight;
        uint32_t seglen = unsent;

        if (seglen > credit)
                seglen = credit;
        if (seglen > chunk_available)
                seglen = chunk_available;
        if (sk->u.tcp.sack.pending.kind == TCP_RECOVERY_TX_NEW_DATA &&
            sk->u.tcp.sack.pending.seq == sk->u.tcp.sent_seq &&
            seglen > sk->u.tcp.sack.pending.end - sk->u.tcp.sent_seq)
                seglen = sk->u.tcp.sack.pending.end - sk->u.tcp.sent_seq;
        if (seglen == 0)
                goto out;

        struct inout_ring *ring = ring_instance();
        const uint8_t *dst_mac = arp_resolve(mp, ring->out, sk->u.tcp.remote_ip,
                                             rte_get_timer_cycles());
        if (dst_mac == NULL) {
                /*
                 * Resolver rate-limits probes.  Park this socket until ARP
                 * learning wakes it instead of retrying every worker turn.
                 * Defer the wait-list move until the packet-building path has
                 * finished using the socket's send state.
                 */
                arp_wait_ip = sk->u.tcp.remote_ip;
                result = TCP_SNDBUF_ARP_WAIT;
                goto out;
        }

        /* Build a stack temporary fragment that points into sndbuf (no free).
         */
        struct tcp_fragment f;
        memset(&f, 0, sizeof(f));
        f.src_port = sk->local_port;
        f.dst_port = sk->u.tcp.remote_port;
        f.sent_seq = sk->u.tcp.sent_seq;
        f.recv_ack = sk->u.tcp.recv_ack;
        f.tcp_flags = RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG;
        f.data_off = (5 << 4);
        f.rx_win = tcp_wire_rcv_wnd(sk, f.tcp_flags);
        (void)tcp_options_apply_established(sk, &f);
        uint32_t mss = tcp_options_data_mss(sk, &f);
        if (seglen > mss)
                seglen = mss;
        if (seglen == 0)
                goto out;
        f.payload = (unsigned char *)payload;
        f.payload_len = seglen;

        struct rte_mbuf *tcp_buf =
            tcp_build_pkt(mp, g_net.local_ip, sk->u.tcp.remote_ip, dst_mac, &f);
        if (tcp_buf == NULL) {
                result = TCP_SNDBUF_RETRY;
                goto out;
        }

        if (rte_ring_sp_enqueue(ring->out, tcp_buf) != 0) {
                LOG_TCP_DEBUG(TCP_SK_FMT " event=tx-drop reason=out-ring-full",
                              TCP_SK_ARG(sk));
                rte_pktmbuf_free(tcp_buf);
                result = TCP_SNDBUF_RETRY;
                goto out;
        }

        LOG_TCP_PACKET(TCP_SK_FMT " event=tx-data seq=%u ack=%u len=%u una=%u",
                       TCP_SK_ARG(sk), f.sent_seq, f.recv_ack, seglen,
                       sk->u.tcp.snd_una);

        /*
         * Measure only the first newly sent data segment in a flight. Its
         * TSval was saved by tcp_options_apply_established() above.
         */
        if (was_idle) {
                tcp_cc_on_idle_restart(&sk->u.tcp, tcp_options_now_ms(),
                                       sk->u.tcp.rto_ms);
                tcp_rtt_note_xmit(sk, f.sent_seq + seglen,
                                  sk->u.tcp.ts_last_val);
        }
        sk->u.tcp.sent_seq += seglen;
        if (sk->u.tcp.sack.pending.kind == TCP_RECOVERY_TX_NEW_DATA)
                tcp_sack_commit_candidate(&sk->u.tcp, sk->u.tcp.sent_seq);
        else
                tcp_sack_note_new_data(&sk->u.tcp, sk->u.tcp.sent_seq);
        sk->u.tcp.sack.limited_transmit = false;
        uint32_t congestion_after =
            UINT32_MAX - congestion_in_flight < seglen
                ? UINT32_MAX
                : congestion_in_flight + seglen;
        struct tcp_cc_tx_event cc_tx = {
            .bytes = seglen,
            .now_ms = tcp_options_now_ms(),
            .retransmission = false,
            .cwnd_limited = tcp_seq_lt(sk->u.tcp.sent_seq, buf_end) &&
                            cwnd <= sk->u.tcp.snd_wnd &&
                            congestion_after >= cwnd,
        };
        tcp_cc_on_packet_sent(&sk->u.tcp, &cc_tx);
        /* Data stays in sndbuf until ACK. */
        if (was_idle) {
                sk->u.tcp.retries = 0;
                tcp_arm_data_rto(sk, sk->u.tcp.rto_ms);
        }
        result = TCP_SNDBUF_SENT;

out:
        if (result == TCP_SNDBUF_ARP_WAIT)
                nsock_tx_arp_wait(sk, arp_wait_ip);
        return result;
}

/**
 * @brief Flush pending control fragments and buffered stream data for a socket.
 * @param sk TCP socket to flush.
 * @param mp Mempool used to create outbound packets and ARP requests.
 * @return 0 after flushing eligible pending work.
 *
 * Control fragments are dequeued from send_buf and freed after TX; application
 * payload is sent from sndbuf and remains retained for ACK/RTO handling.
 */
int tcp_tx_flush(struct nsock *sk, struct rte_mempool *mp) {
        /* States that may still have outbound segments on send_buf. */
        switch (sk->u.tcp.status) {
        case TCP_STATUS_SYN_SENT: /* active-open SYN (+ retransmits) */
        case TCP_STATUS_SYN_RECV:
        case TCP_STATUS_ESTABLISHED:
        case TCP_STATUS_CLOSE_WAIT: /* ACK of peer FIN may still be queued */
        case TCP_STATUS_LAST_ACK:   /* our FIN after passive close */
        case TCP_STATUS_FIN_WAIT_1: /* our FIN after active close */
        case TCP_STATUS_FIN_WAIT_2:
        case TCP_STATUS_TIME_WAIT:
        case TCP_STATUS_CLOSING:
                break;
        default:
                return SOCK_TX_FLUSH_IDLE;
        }

        /*
         * Outer loop lets a deferred FIN be transmitted immediately after the
         * last congestion/window-limited payload segment.
         * sent_seq advances on enqueue, so the FIN condition cannot loop.
         */
        for (;;) {
                /* Drain send_buf so ACK-of-FIN + our FIN leave in the same
                 * pass. */
                for (;;) {
                        struct tcp_fragment *f = NULL;
                        f = nsock_tcp_tx_dequeue(sk);
                        if (f == NULL)
                                break;

                        uint32_t peer_ip = sk->u.tcp.remote_ip;
                        struct inout_ring *ring = ring_instance();
                        const uint8_t *dst_mac = arp_resolve(
                            mp, ring->out, peer_ip, rte_get_timer_cycles());
                        if (dst_mac == NULL) {
                                LOG_TCP_DEBUG(
                                    TCP_SK_FMT
                                    " event=tx-wait-arp flags=%s seq=%u "
                                    "ack=%u",
                                    TCP_SK_ARG(sk), tcp_flags_str(f->tcp_flags),
                                    f->sent_seq, f->recv_ack);
                                if (nsock_tcp_tx_requeue_head(sk, f) != 0)
                                        tcp_fragment_free(f);
                                else {
                                        nsock_tx_arp_wait(sk, peer_ip);
                                        return SOCK_TX_FLUSH_ARP_WAIT;
                                }
                                return SOCK_TX_FLUSH_IDLE;
                        }

                        struct rte_mbuf *tcp_buf = tcp_build_pkt(
                            mp, g_net.local_ip, peer_ip, dst_mac, f);
                        if (tcp_buf == NULL) {
                                if (nsock_tcp_tx_requeue_head(sk, f) != 0)
                                        tcp_fragment_free(f);
                                else
                                        return SOCK_TX_FLUSH_RETRY;
                                return SOCK_TX_FLUSH_IDLE;
                        }

                        if (rte_ring_sp_enqueue(ring->out, tcp_buf) != 0) {
                                LOG_TCP_DEBUG(
                                    TCP_SK_FMT
                                    " event=tx-drop reason=out-ring-full",
                                    TCP_SK_ARG(sk));
                                rte_pktmbuf_free(tcp_buf);
                                if (nsock_tcp_tx_requeue_head(sk, f) != 0)
                                        tcp_fragment_free(f);
                                else
                                        return SOCK_TX_FLUSH_RETRY;
                                return SOCK_TX_FLUSH_IDLE;
                        }
                        /*
                         * App data uses sndbuf + data RTO (kept until ACK).
                         * Control segments on send_buf (SYN / SYN+ACK / FIN)
                         * are freed here after TX; independent RTOs re-queue
                         * them: SYN_SENT, SYN_RECV, and FIN_WAIT_1 / LAST_ACK /
                         * CLOSING.
                         */

                        if ((f->tcp_flags &
                             (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG)) ==
                            (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG)) {
                                LOG_TCP_INFO(
                                    TCP_SK_FMT
                                    " event=handshake step=2/3 tx=SYN-ACK "
                                    "seq=%u ack=%u",
                                    TCP_SK_ARG(sk), f->sent_seq, f->recv_ack);
                        } else {
                                LOG_TCP_PACKET(
                                    TCP_SK_FMT
                                    " event=tx-control flags=%s seq=%u "
                                    "ack=%u len=%zu",
                                    TCP_SK_ARG(sk), tcp_flags_str(f->tcp_flags),
                                    f->sent_seq, f->recv_ack, f->payload_len);
                        }

                        tcp_fragment_free(f);
                }

                enum tcp_sndbuf_flush_result recovery_result;
                do {
                        recovery_result =
                            tcp_tx_flush_recovery_retransmit(sk, mp);
                } while (recovery_result == TCP_SNDBUF_SENT);
                if (recovery_result == TCP_SNDBUF_ARP_WAIT)
                        return SOCK_TX_FLUSH_ARP_WAIT;
                if (recovery_result == TCP_SNDBUF_RETRY)
                        return SOCK_TX_FLUSH_RETRY;

                enum tcp_sndbuf_flush_result sndbuf_result;
                sndbuf_result = tcp_tx_flush_sndbuf(sk, mp);
                if (sndbuf_result == TCP_SNDBUF_ARP_WAIT)
                        return SOCK_TX_FLUSH_ARP_WAIT;
                if (sndbuf_result == TCP_SNDBUF_RETRY)
                        return SOCK_TX_FLUSH_RETRY;
                if (sndbuf_result == TCP_SNDBUF_SENT)
                        continue;

                /* A congestion/window-limited close defers FIN until every
                 * buffered payload byte has received a sequence number. */
                if ((sk->u.tcp.status == TCP_STATUS_FIN_WAIT_1 ||
                     sk->u.tcp.status == TCP_STATUS_LAST_ACK ||
                     sk->u.tcp.status == TCP_STATUS_CLOSING) &&
                    sk->u.tcp.fin_deferred &&
                    sk->u.tcp.sent_seq ==
                        sk->u.tcp.snd_una + sk->u.tcp.sndbuf.len) {
                        struct tcp_fragment *fin_f = tcp_make_fragment(
                            sk, RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG,
                            sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
                        if (tcp_enqueue_fragment(sk, fin_f) == 0) {
                                sk->u.tcp.sent_seq += 1;
                                sk->u.tcp.fin_deferred = false;
                                tcp_arm_data_rto(sk, sk->u.tcp.rto_ms);
                                LOG_INFO("tcp FIN queue after data " TCP_ID_FMT
                                         " seq=%u",
                                         TCP_ID_ARG(sk),
                                         sk->u.tcp.sent_seq - 1);
                                continue;
                        }
                }
                return SOCK_TX_FLUSH_IDLE;
        }
}

/**
 * @brief Queue application bytes for reliable TCP transmission with
 * backpressure.
 * @param sk Established TCP socket.
 * @param buf Source application buffer.
 * @param len Requested byte count.
 * @param flags @c MSG_DONTWAIT requests non-blocking behavior.
 * @return Immediately accepted byte count, 0 for an empty request, or -1 with
 *         errno EAGAIN/EPIPE/ENOBUFS.  The owner command layer implements
 *         blocking behavior by parking and retrying EAGAIN requests.
 *
 * This function must never sleep: it runs on the packet worker, which must
 * continue processing ACK/window updates to make send space available.
 */
ssize_t tcp_send(struct nsock *sk, const void *buf, size_t len, int flags) {
        (void)flags;

        if (len == 0)
                return 0;

        if (sk->u.tcp.status != TCP_STATUS_ESTABLISHED) {
                errno = EPIPE;
                return -1;
        }

        uint32_t space = tcp_app_snd_space(sk);
        if (space == 0) {
                errno = EAGAIN;
                return -1;
        }

        size_t accepted = len < space ? len : space;
        ssize_t n = tcp_sndbuf_append(&sk->u.tcp.sndbuf, buf, accepted);

        if (n <= 0) {
                errno = ENOBUFS;
                return -1;
        }

        tcp_mark_tx_dirty(sk);
        LOG_TCP_DEBUG(TCP_SK_FMT
                      " event=send-queued len=%zd sndbuf=%u una=%u nxt=%u",
                      TCP_SK_ARG(sk), n, sk->u.tcp.sndbuf.len,
                      sk->u.tcp.snd_una, sk->u.tcp.sent_seq);
        return n;
}

/**
 * @brief Receive contiguous TCP payload bytes for an application.
 * @param sk TCP socket whose receive queue is consumed.
 * @param buf Destination application buffer.
 * @param len Destination capacity.
 * @param flags Reserved receive flags; currently ignored.
 * @return Number of bytes copied, 0 for orderly EOF, or -1/EAGAIN when the
 *         owner must park a blocking RECV command.
 *
 * Short reads retain the unread suffix in owner-held @c rx_current so the next
 * command preserves TCP byte-stream order. Application lcores never consume
 * @c recv_buf directly.
 */
ssize_t tcp_recv(struct nsock *sk, void *buf, size_t len,
                 __attribute__((unused)) int flags) {
        struct tcp_rx_blob *b = sk->u.tcp.rx_current;

        if (len == 0)
                return 0;

        if (b == NULL) {
                b = nsock_tcp_rx_dequeue(sk);
                if (b == NULL) {
                        /*
                         * CLOSE_WAIT means the peer FIN has been consumed.
                         * Once all previously queued bytes are drained, the
                         * stream reports the conventional zero-length EOF.
                         */
                        if (sk->u.tcp.status == TCP_STATUS_CLOSE_WAIT ||
                            sk->u.tcp.status == TCP_STATUS_CLOSED)
                                return 0;
                        errno = EAGAIN;
                        return -1;
                }
                sk->u.tcp.rx_current = b;
        }

        size_t avail = b->len - b->off;
        size_t n = len < avail ? len : avail;
        rte_memcpy(buf, b->data + b->off, n);
        b->off += n;

        /*
         * The owner performs the copy and receive accounting itself; no raw
         * nsock pointer crosses from the application lcore.
         */
        if (n > sk->u.tcp.rcvbuf_used)
                sk->u.tcp.rcvbuf_used = 0;
        else
                sk->u.tcp.rcvbuf_used -= (uint32_t)n;
        tcp_ofo_drain(sk, tcp_deliver_payload, tcp_ofo_enter_close_wait);

        /* Promptly advertise space, including recovery from a zero window. */
        struct tcp_fragment *ack_f = tcp_make_fragment(
            sk, RTE_TCP_ACK_FLAG, sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
        uint16_t ack_win = ack_f == NULL ? 0 : ack_f->rx_win;
        (void)tcp_enqueue_fragment(sk, ack_f);
        LOG_TCP_DEBUG(TCP_SK_FMT
                      " event=ack-queue reason=window-update ack=%u win=%u "
                      "app_read=%zu rcvbuf_used=%u",
                      TCP_SK_ARG(sk), sk->u.tcp.recv_ack, ack_win, n,
                      sk->u.tcp.rcvbuf_used);

        if (b->off < b->len)
                return (ssize_t)n;

        tcp_payload_release(b->data, b->storage);
        if (tcp_memory_current() != NULL)
                tcp_memory_rx_blob_free(tcp_memory_current(), b);
        else
                rte_free(b);
        sk->u.tcp.rx_current = NULL;
        return (ssize_t)n;
}

/** @brief Discard pending control fragments and reset send-side sequence state.
 *
 * Runs on the socket owner, drains queued control fragments, resets the data
 * window to @c sent_seq, and lets the owning command path notify waiters.
 *
 * @param sk Socket whose transmit-side state is reset.
 */
static void tcp_drain_send(struct nsock *sk) {
        struct tcp_fragment *f;

        while ((f = nsock_tcp_tx_dequeue(sk)) != NULL) {
                tcp_fragment_free(f);
        }
        tcp_sndbuf_reset(&sk->u.tcp.sndbuf, sk->u.tcp.sent_seq);
        sk->u.tcp.snd_una = sk->u.tcp.sent_seq;
        tcp_sack_score_clear(sk);
}

/** @brief Release queued application payload and all out-of-order data.
 * @param sk Socket whose receive-side state is discarded.
 */
static void tcp_drain_recv(struct nsock *sk) {
        struct tcp_rx_blob *b;
        while ((b = nsock_tcp_rx_dequeue(sk)) != NULL) {
                tcp_payload_release(b->data, b->storage);
                if (tcp_memory_current() != NULL)
                        tcp_memory_rx_blob_free(tcp_memory_current(), b);
                else
                        rte_free(b);
        }
        b = sk->u.tcp.rx_current;
        if (b != NULL) {
                tcp_payload_release(b->data, b->storage);
                if (tcp_memory_current() != NULL)
                        tcp_memory_rx_blob_free(tcp_memory_current(), b);
                else
                        rte_free(b);
                sk->u.tcp.rx_current = NULL;
        }
        tcp_ofo_purge(sk);
        sk->u.tcp.rcvbuf_used = 0;
}

/**
 * @brief Start TCP teardown without blocking the owning packet worker.
 *
 * nclose() has already detached the application fd and marked app_closed
 * before entering here.  This function either destroys an immediately
 * reclaimable socket or starts FIN/RTO processing and returns.  Terminal
 * packet/timer handlers perform final reclamation later.
 */
int tcp_close(struct nsock *sk) {
        TCP_STATUS status = sk->u.tcp.status;
        LOG_TCP_INFO(TCP_SK_FMT " event=close-request status=%s",
                     TCP_SK_ARG(sk), tcp_status_str(status));

        if (status == TCP_STATUS_CLOSED) {
                tcp_drain_send(sk);
                tcp_drain_recv(sk);
                nsock_free(sk);
                return 0;
        }

        if (status == TCP_STATUS_LISTEN) {
                /*
                 * Completed but unaccepted children are owned by the accept
                 * queue.  Accepted children survive listener close; sever
                 * their parent pointer before freeing the listener.
                 */
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

                struct nsock *cur = nsock_list_local();
                while (cur != NULL) {
                        struct nsock *next = cur->next;
                        if (cur != sk && cur->protocol == IPPROTO_TCP &&
                            cur->u.tcp.listener == sk) {
                                if (cur->app_visible) {
                                        cur->u.tcp.listener = NULL;
                                } else {
                                        tcp_drain_send(cur);
                                        tcp_drain_recv(cur);
                                        nsock_free(cur);
                                }
                        }
                        cur = next;
                }

                sk->u.tcp.syn_pending = 0;
                tcp_drain_send(sk);
                tcp_drain_recv(sk);
                nsock_free(sk);
                return 0;
        }

        if (status == TCP_STATUS_SYN_SENT) {
                rte_timer_stop(&sk->u.tcp.timer);
                tcp_stream_set_status(sk, TCP_STATUS_CLOSED);
                socket_owner_complete_connect(sk, ECANCELED);
                tcp_drain_send(sk);
                tcp_drain_recv(sk);
                nsock_free(sk);
                return 0;
        }

        if (status == TCP_STATUS_ESTABLISHED || status == TCP_STATUS_SYN_RECV) {
                if (status == TCP_STATUS_SYN_RECV &&
                    sk->u.tcp.listener != NULL &&
                    sk->u.tcp.listener->u.tcp.syn_pending > 0)
                        sk->u.tcp.listener->u.tcp.syn_pending--;

                uint32_t buf_end = sk->u.tcp.sndbuf.head_seq +
                                   sk->u.tcp.sndbuf.len;
                if (tcp_seq_lt(sk->u.tcp.sent_seq, buf_end)) {
                        sk->u.tcp.fin_deferred = true;
                } else {
                        struct tcp_fragment *fin_f = tcp_make_fragment(
                            sk, RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG,
                            sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
                        if (tcp_enqueue_fragment(sk, fin_f) != 0)
                                goto fin_enqueue_failed;
                        sk->u.tcp.sent_seq++;
                }
                sk->u.tcp.retries = 0;
                if (sk->u.tcp.snd_una != sk->u.tcp.sent_seq)
                        tcp_arm_data_rto(sk, sk->u.tcp.rto_ms);
                tcp_stream_set_status(sk, TCP_STATUS_FIN_WAIT_1);
                if (sk->u.tcp.fin_deferred)
                        tcp_mark_tx_dirty(sk);
                return 0;
        }

        if (status == TCP_STATUS_CLOSE_WAIT) {
                uint32_t buf_end = sk->u.tcp.sndbuf.head_seq +
                                   sk->u.tcp.sndbuf.len;
                if (tcp_seq_lt(sk->u.tcp.sent_seq, buf_end)) {
                        sk->u.tcp.fin_deferred = true;
                } else {
                        struct tcp_fragment *fin_f = tcp_make_fragment(
                            sk, RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG,
                            sk->u.tcp.sent_seq, sk->u.tcp.recv_ack);
                        if (tcp_enqueue_fragment(sk, fin_f) != 0)
                                goto fin_enqueue_failed;
                        sk->u.tcp.sent_seq++;
                }
                sk->u.tcp.retries = 0;
                if (sk->u.tcp.snd_una != sk->u.tcp.sent_seq)
                        tcp_arm_data_rto(sk, sk->u.tcp.rto_ms);
                tcp_stream_set_status(sk, TCP_STATUS_LAST_ACK);
                tcp_drain_recv(sk);
                if (sk->u.tcp.fin_deferred)
                        tcp_mark_tx_dirty(sk);
                return 0;
        }

        /*
         * FIN_WAIT_*, CLOSING, LAST_ACK, and TIME_WAIT already have protocol
         * teardown in flight.  app_closed ensures their eventual terminal
         * handler reclaims the object.
         */
        return 0;

fin_enqueue_failed:
        /*
         * The fd is already gone, so leaving an unreachable TCB would leak
         * forever.  Abort locally on allocation/ring failure; a future RST
         * enhancement can notify the peer explicitly.
         */
        LOG_ERROR("tcp_close: FIN enqueue failed socket=%u; abort", sk->id);
        tcp_stream_set_status(sk, TCP_STATUS_CLOSED);
        tcp_drain_send(sk);
        tcp_drain_recv(sk);
        nsock_free(sk);
        errno = ENOBUFS;
        return -1;
}

/**
 * @brief Start an owner-driven active TCP open without blocking the worker.
 *
 * Active open: CLOSED -> SYN_SENT -> (wait) ESTABLISHED.
 * If the socket is unbound, fill local_ip from g_net and allocate an ephemeral
 * local_port (BSD-style implicit bind).  On successful start it returns
 * -1/EINPROGRESS; the owner parks the CONNECT command and the SYN response,
 * RTO, RST, or close path completes it later.
 * @param sk CLOSED TCP socket to connect.
 * @param addr Peer IPv4 socket address.
 * @param addrlen Address length; currently ignored.
 * @return -1/EINPROGRESS after starting, or -1 with another errno on setup
 *         failure.
 */
int tcp_connect(struct nsock *sk, const struct sockaddr *addr,
                __attribute__((unused)) socklen_t addrlen) {
        if (addr == NULL)
                return -1;
        if (sk->u.tcp.status != TCP_STATUS_CLOSED) {
                LOG_ERROR("tcp_connect: " TCP_ID_FMT " status=%s",
                          TCP_ID_ARG(sk), tcp_status_str(sk->u.tcp.status));
                return -1;
        }

        const struct sockaddr_in *peer = (const struct sockaddr_in *)addr;
        /*
         * The remote half of the four-tuple is needed before implicit-port
         * allocation: under RSS we choose a source port whose return traffic
         * hashes back to this socket's owner queue.
         */
        sk->u.tcp.remote_ip = peer->sin_addr.s_addr;
        sk->u.tcp.remote_port = peer->sin_port;

        /* Implicit bind when the app skipped nbind (typical client path). */
        if (sk->local_ip == 0) {
                sk->local_ip = g_net.local_ip;
        }
        if (sk->local_port == 0) {
                sk->local_port = tcp_alloc_ephemeral_port(sk);
                if (sk->local_port == 0) {
                        errno = EADDRNOTAVAIL;
                        LOG_ERROR("tcp_connect: " TCP_ID_FMT
                                  " local port allocation failed",
                                  TCP_ID_ARG(sk));
                        return -1;
                }
                int bind_rc =
                    nsock_bind_local(sk, g_net.local_ip, sk->local_port);
                if (bind_rc != 0) {
                        errno = -bind_rc;
                        return -1;
                }
        }

        sk->u.tcp.listener = NULL;
        sk->u.tcp.sent_seq = tcp_next_isn();
        sk->u.tcp.snd_una = sk->u.tcp.sent_seq;
        tcp_sndbuf_reset(&sk->u.tcp.sndbuf, sk->u.tcp.sent_seq);

        sk->u.tcp.recv_ack = 0;
        sk->u.tcp.retries = 0;

        sk->u.tcp.rcv_wscale = tcp_choose_rcv_wscale(sk->u.tcp.rcvbuf_size);
        sk->u.tcp.snd_wscale = 0;
        sk->u.tcp.wscale_ok = false;

        tcp_options_reset_state(sk);
        tcp_rtt_reset(sk);
        sk->u.tcp.syn_retransmitted = false;
        tcp_cc_init_default(&sk->u.tcp, false);

        if (nsock_tcp_conn_register(sk) != 0) {
                LOG_ERROR("tcp_connect: duplicate 4-tuple");
                return -1;
        }

        struct tcp_fragment *syn_f =
            tcp_make_fragment(sk, RTE_TCP_SYN_FLAG, sk->u.tcp.sent_seq, 0);
        if (syn_f != NULL)
                (void)tcp_options_apply_syn(sk, syn_f, true, true, true, 0);
        if (tcp_enqueue_fragment(sk, syn_f) != 0) {
                LOG_ERROR("tcp_connect: SYN enqueue failed " TCP_ID_FMT,
                          TCP_ID_ARG(sk));
                nsock_tcp_conn_unregister(sk);
                return -1;
        }

        tcp_stream_set_status(sk, TCP_STATUS_SYN_SENT);
        tcp_arm_syn_timer(sk, TCP_SYN_RTO_MS);

        LOG_TCP_INFO("tcp connect SYN_SENT " TCP_ID_FMT " " IP_FMT
                     ":%u -> " IP_FMT ":%u",
                     TCP_ID_ARG(sk), IP_ARG(sk->local_ip),
                     rte_be_to_cpu_16(sk->local_port),
                     IP_ARG(sk->u.tcp.remote_ip),
                     rte_be_to_cpu_16(sk->u.tcp.remote_port));

        errno = EINPROGRESS;
        return -1;
}

/**
 * @brief Put a bound TCP socket into LISTEN state.
 * @param sk Socket to configure as a listener.
 * @param backlog Maximum incomplete plus completed pending connections.
 * @return 0 on success, or -1 if the accept queue cannot be allocated.
 */
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
        /*
         * Owner-created sockets intentionally have no embedded fd.  Use the
         * slot generation so multiple listeners receive unique DPDK object
         * names even when application fd numbers are reused.
         */
        snprintf(name, sizeof(name), "tcp_accept_%u_%u", sk->id,
                 sk->generation);
        sk->u.tcp.accept_queue =
            rte_ring_create(name, ring_sz, rte_socket_id(), 0);
        if (sk->u.tcp.accept_queue == NULL) {
                LOG_ERROR("tcp_listen: accept_queue create failed " TCP_ID_FMT
                          " "
                          "backlog=%d ring_sz=%u",
                          TCP_ID_ARG(sk), backlog, ring_sz);
                sk->u.tcp.status = TCP_STATUS_CLOSED;
                return -1;
        }
        if (nsock_tcp_listener_register(sk) != 0) {
                LOG_ERROR("tcp_listen: local endpoint already listening");
                rte_ring_free(sk->u.tcp.accept_queue);
                sk->u.tcp.accept_queue = NULL;
                sk->u.tcp.status = TCP_STATUS_CLOSED;
                return -1;
        }
        LOG_TCP_INFO(TCP_SK_FMT " event=listen backlog=%d ring_sz=%u",
                     TCP_SK_ARG(sk), backlog, ring_sz);
        return 0;
}

/**
 * @brief Block until an established passive-open child can be accepted.
 * @param sk Listening socket.
 * @param addr Optional output buffer for the peer IPv4 address and port.
 * @param addrlen Address length pointer; currently ignored.
 * @return Newly assigned child fd, or -1 on invalid listener or fd exhaustion.
 *
 * The child receives an fd only after the three-way handshake completed.
 */
struct nsock *tcp_accept_owned(struct nsock *sk) {
        if (sk->u.tcp.status != TCP_STATUS_LISTEN ||
            sk->u.tcp.accept_queue == NULL) {
                errno = EINVAL;
                return NULL;
        }

        for (;;) {
                struct nsock *child = NULL;
                if (rte_ring_sc_dequeue(sk->u.tcp.accept_queue,
                                        (void **)&child) != 0) {
                        errno = EAGAIN;
                        return NULL;
                }

                /*
                 * A reset can mark a completed child CLOSED while it is still
                 * queued.  It remains allocated until this owner dequeues it,
                 * preventing a dangling accept_queue pointer.
                 */
                if (child->u.tcp.status == TCP_STATUS_CLOSED) {
                        LOG_INFO("tcp_accept discard reset child peer " IP_FMT
                                 ":%u",
                                 IP_ARG(child->u.tcp.remote_ip),
                                 rte_be_to_cpu_16(child->u.tcp.remote_port));
                        tcp_drain_send(child);
                        tcp_drain_recv(child);
                        nsock_free(child);
                        continue;
                }

                LOG_INFO("tcp_accept listener=%u -> child=%u peer " IP_FMT
                         ":%u",
                         sk->id, child->id, IP_ARG(child->u.tcp.remote_ip),
                         rte_be_to_cpu_16(child->u.tcp.remote_port));
                return child;
        }
}

/*
 * The generic ops table keeps an accept capability marker, but actual accept
 * execution is handled by tcp_accept_owned() in socket_owner.c so it can
 * return a generation-checked child handle instead of an owner pointer/fd.
 */
int tcp_accept(__attribute__((unused)) struct nsock *sk,
               __attribute__((unused)) struct sockaddr *addr,
               __attribute__((unused)) socklen_t *addrlen) {
        errno = EOPNOTSUPP;
        return -1;
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
    .connect = tcp_connect,
    .listen = tcp_listen,
    .accept = tcp_accept,
};
