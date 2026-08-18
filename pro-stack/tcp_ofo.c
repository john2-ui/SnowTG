/**
 * @file tcp_ofo.c
 * @brief TCP out-of-order reassembly, accounting, and pressure admission.
 */

#include "tcp_ofo.h"

#include "config.h"
#include "log.h"
#include "socket.h"
#include "socket_owner_internal.h"
#include "tcp.h"
#include "tcp_memory.h"

#include <limits.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <string.h>

#define TCP_OFO_ID_FMT "sock=%u gen=%u"
#define TCP_OFO_ID_ARG(sk) (sk)->id, (sk)->generation

/** Mutable OFO policy and interval metrics owned by one packet worker. */
struct tcp_ofo_owner_state {
        struct tcp_ofo_metrics metrics;
        bool pressure;
#ifdef TCP_TESTING
        bool force_pressure_set;
        bool force_pressure;
        bool fail_next_alloc;
        uint64_t owner_limit_override;
#endif
};

static struct tcp_ofo_owner_state g_tcp_ofo_state[RTE_MAX_LCORE];

static inline int tcp_ofo_seq_lt(uint32_t a, uint32_t b) {
        return (int32_t)(a - b) < 0;
}

static inline int tcp_ofo_seq_leq(uint32_t a, uint32_t b) {
        return (int32_t)(a - b) <= 0;
}

static inline int tcp_ofo_seq_gt(uint32_t a, uint32_t b) {
        return (int32_t)(a - b) > 0;
}

static uint32_t tcp_ofo_rcv_wnd(const struct nsock *sk) {
        if (sk->u.tcp.rcvbuf_used >= sk->u.tcp.rcvbuf_size)
                return 0;
        return sk->u.tcp.rcvbuf_size - sk->u.tcp.rcvbuf_used;
}

#ifdef TCP_TESTING
static inline struct tcp_owner_memory *tcp_ofo_memory_current(void) {
        return NULL;
}
#else
static struct tcp_owner_memory *tcp_ofo_memory_current(void) {
        return socket_owner_tcp_memory();
}
#endif

static struct tcp_ofo_owner_state *tcp_ofo_state_current(void) {
        unsigned int lcore_id = rte_lcore_id();

        return lcore_id < RTE_MAX_LCORE ? &g_tcp_ofo_state[lcore_id] : NULL;
}

static uint64_t
tcp_ofo_owner_limit(const struct tcp_ofo_owner_state *state) {
#ifdef TCP_TESTING
        if (state != NULL && state->owner_limit_override != 0)
                return state->owner_limit_override;
#else
        (void)state;
#endif
        return TCP_OFO_OWNER_MAX_BYTES;
}

static void tcp_ofo_set_pressure(struct tcp_ofo_owner_state *state,
                                 bool pressure) {
        if (state == NULL || state->pressure == pressure)
                return;
        state->pressure = pressure;
        state->metrics.pressure_transitions++;
}

/** Refresh the pressure latch using owner-pool and OFO-byte hysteresis. */
static void tcp_ofo_update_pressure(struct tcp_ofo_owner_state *state) {
        struct tcp_owner_memory *memory;
        uint64_t owner_limit;
        uint64_t enter_bytes;
        uint64_t exit_bytes;
        uint32_t ofo_available = UINT32_MAX;
        uint32_t payload_available = UINT32_MAX;

        if (state == NULL)
                return;
#ifdef TCP_TESTING
        if (state->force_pressure_set) {
                tcp_ofo_set_pressure(state, state->force_pressure);
                return;
        }
#endif
        memory = tcp_ofo_memory_current();
        if (memory != NULL) {
                if (memory->pools[TCP_MEMORY_OFO_SEG] != NULL)
                        ofo_available = rte_mempool_avail_count(
                            memory->pools[TCP_MEMORY_OFO_SEG]);
                if (memory->pools[TCP_MEMORY_PAYLOAD] != NULL)
                        payload_available = rte_mempool_avail_count(
                            memory->pools[TCP_MEMORY_PAYLOAD]);
        }
        owner_limit = tcp_ofo_owner_limit(state);
        enter_bytes = owner_limit * TCP_OFO_PRESSURE_ENTER_PERCENT / 100U;
        exit_bytes = owner_limit * TCP_OFO_PRESSURE_EXIT_PERCENT / 100U;

        if (!state->pressure) {
                if (ofo_available <= TCP_MEMORY_LOW_WATER ||
                    payload_available <= TCP_MEMORY_LOW_WATER ||
                    state->metrics.bytes_current >= enter_bytes)
                        tcp_ofo_set_pressure(state, true);
                return;
        }
        if (ofo_available >= TCP_MEMORY_HIGH_WATER &&
            payload_available >= TCP_MEMORY_HIGH_WATER &&
            state->metrics.bytes_current <= exit_bytes)
                tcp_ofo_set_pressure(state, false);
}

void tcp_ofo_metrics_reset_owner(unsigned int lcore_id) {
        if (lcore_id < RTE_MAX_LCORE)
                memset(&g_tcp_ofo_state[lcore_id], 0,
                       sizeof(g_tcp_ofo_state[lcore_id]));
}

void tcp_ofo_metrics_take(struct tcp_ofo_metrics *out) {
        struct tcp_ofo_owner_state *state;
        uint64_t segments_current;
        uint64_t bytes_current;

        if (out == NULL)
                return;
        state = tcp_ofo_state_current();
        if (state == NULL) {
                memset(out, 0, sizeof(*out));
                return;
        }
        tcp_ofo_update_pressure(state);
        state->metrics.pressure_active = state->pressure ? 1U : 0U;
        *out = state->metrics;

        segments_current = state->metrics.segments_current;
        bytes_current = state->metrics.bytes_current;
        memset(&state->metrics, 0, sizeof(state->metrics));
        state->metrics.segments_current = segments_current;
        state->metrics.segments_peak = segments_current;
        state->metrics.bytes_current = bytes_current;
        state->metrics.bytes_peak = bytes_current;
        state->metrics.pressure_active = state->pressure ? 1U : 0U;
}

static int tcp_ofo_owner_can_reserve(struct tcp_ofo_owner_state *state,
                                     uint32_t bytes) {
        uint64_t limit;

        if (bytes == 0)
                return 0;
        if (state == NULL)
                return -1;
        limit = tcp_ofo_owner_limit(state);
        return state->metrics.bytes_current > limit ||
                       bytes > limit - state->metrics.bytes_current
                   ? -1
                   : 0;
}

static void tcp_ofo_metrics_release(uint32_t bytes, bool segment) {
        struct tcp_ofo_owner_state *state = tcp_ofo_state_current();

        if (state == NULL)
                return;
        if (bytes >= state->metrics.bytes_current)
                state->metrics.bytes_current = 0;
        else
                state->metrics.bytes_current -= bytes;
        state->metrics.released_bytes += bytes;
        if (segment) {
                if (state->metrics.segments_current != 0)
                        state->metrics.segments_current--;
                state->metrics.released_segments++;
        }
}

struct tcp_ofo_limits {
        uint32_t segments;
        uint32_t bytes;
};

static struct tcp_ofo_limits
tcp_ofo_effective_limits(struct tcp_ofo_owner_state *state,
                         uint32_t reorder_distance) {
        struct tcp_ofo_limits limits = {
            .segments = TCP_OFO_MAX_SEGS,
            .bytes = TCP_OFO_MAX_BYTES,
        };

        tcp_ofo_update_pressure(state);
        if (state == NULL || !state->pressure)
                return limits;
        if (reorder_distance <= TCP_OFO_PRESSURE_NEAR_DISTANCE) {
                limits.segments = TCP_OFO_PRESSURE_NEAR_MAX_SEGS;
                limits.bytes = TCP_OFO_PRESSURE_NEAR_MAX_BYTES;
        } else if (reorder_distance <= TCP_OFO_PRESSURE_MEDIUM_DISTANCE) {
                limits.segments = TCP_OFO_PRESSURE_MEDIUM_MAX_SEGS;
                limits.bytes = TCP_OFO_PRESSURE_MEDIUM_MAX_BYTES;
        } else {
                limits.segments = TCP_OFO_PRESSURE_FAR_MAX_SEGS;
                limits.bytes = TCP_OFO_PRESSURE_FAR_MAX_BYTES;
        }
        return limits;
}

static void tcp_ofo_metrics_accept(struct tcp_ofo_owner_state *state,
                                   uint32_t bytes) {
        if (state == NULL)
                return;
        state->metrics.segments_current++;
        state->metrics.bytes_current += bytes;
        state->metrics.accepted_segments++;
        state->metrics.accepted_bytes += bytes;
        if (state->metrics.segments_current > state->metrics.segments_peak)
                state->metrics.segments_peak =
                    state->metrics.segments_current;
        if (state->metrics.bytes_current > state->metrics.bytes_peak)
                state->metrics.bytes_peak = state->metrics.bytes_current;
}

static void tcp_ofo_rcvbuf_sub(struct nsock *sk, uint32_t bytes) {
        if (bytes >= sk->u.tcp.rcvbuf_used)
                sk->u.tcp.rcvbuf_used = 0;
        else
                sk->u.tcp.rcvbuf_used -= bytes;
}

static struct tcp_ofo_seg *tcp_ofo_lower_bound(const struct nsock *sk,
                                               uint32_t seq) {
        struct rb_node *node = sk->u.tcp.ofo_tree.node;
        struct tcp_ofo_seg *result = NULL;

        while (node != NULL) {
                struct tcp_ofo_seg *cur =
                    rb_entry(node, struct tcp_ofo_seg, rb);

                if (tcp_ofo_seq_lt(cur->seq, seq)) {
                        node = node->right;
                } else {
                        result = cur;
                        node = node->left;
                }
        }
        return result;
}

static bool tcp_ofo_range_buffered(const struct nsock *sk, uint32_t seq,
                                   uint32_t len, bool has_fin) {
        struct tcp_ofo_seg *node = tcp_ofo_lower_bound(sk, seq);
        struct tcp_ofo_seg *last = NULL;
        uint32_t end = seq + len;
        uint32_t cursor = seq;

        if (node != NULL && node->prev != NULL &&
            tcp_ofo_seq_gt(node->prev->seq + node->prev->len, seq))
                node = node->prev;
        else if (node == NULL && sk->u.tcp.ofo_tail != NULL &&
                 tcp_ofo_seq_gt(sk->u.tcp.ofo_tail->seq +
                                    sk->u.tcp.ofo_tail->len,
                                seq))
                node = sk->u.tcp.ofo_tail;

        while (node != NULL && tcp_ofo_seq_lt(cursor, end)) {
                uint32_t node_end = node->seq + node->len;

                if (tcp_ofo_seq_gt(node->seq, cursor))
                        return false;
                if (tcp_ofo_seq_gt(node_end, cursor))
                        cursor = node_end;
                last = node;
                node = node->next;
        }
        if (tcp_ofo_seq_lt(cursor, end))
                return false;
        if (!has_fin)
                return true;
        if (last != NULL && last->has_fin && last->seq + last->len == end)
                return true;
        node = tcp_ofo_lower_bound(sk, end);
        return node != NULL && node->seq == end && node->len == 0 &&
               node->has_fin;
}

void tcp_ofo_record_rcv_window_drop(struct nsock *sk, uint32_t seq,
                                    uint32_t len, bool has_fin) {
        struct tcp_ofo_owner_state *state = tcp_ofo_state_current();

        if (state != NULL &&
            !tcp_ofo_range_buffered(sk, seq, len, has_fin))
                state->metrics.drop_rcvbuf++;
}

static void tcp_ofo_tree_insert(struct nsock *sk, struct tcp_ofo_seg *seg) {
        struct rb_node **link = &sk->u.tcp.ofo_tree.node;
        struct rb_node *parent = NULL;

        while (*link != NULL) {
                struct tcp_ofo_seg *cur =
                    rb_entry(*link, struct tcp_ofo_seg, rb);

                parent = *link;
                if (tcp_ofo_seq_lt(seg->seq, cur->seq))
                        link = &(*link)->left;
                else
                        link = &(*link)->right;
        }
        rb_link_node(&seg->rb, parent, link);
        rb_insert_color(&seg->rb, &sk->u.tcp.ofo_tree);
}

static void tcp_ofo_list_insert_before(struct nsock *sk,
                                       struct tcp_ofo_seg *seg,
                                       struct tcp_ofo_seg *before) {
        if (before == NULL) {
                seg->next = NULL;
                seg->prev = sk->u.tcp.ofo_tail;
                if (sk->u.tcp.ofo_tail != NULL)
                        sk->u.tcp.ofo_tail->next = seg;
                else
                        sk->u.tcp.ofo = seg;
                sk->u.tcp.ofo_tail = seg;
                return;
        }
        seg->next = before;
        seg->prev = before->prev;
        if (before->prev != NULL)
                before->prev->next = seg;
        else
                sk->u.tcp.ofo = seg;
        before->prev = seg;
}

static void tcp_ofo_unlink(struct nsock *sk, struct tcp_ofo_seg *seg) {
        rb_erase(&seg->rb, &sk->u.tcp.ofo_tree);
        if (seg->prev != NULL)
                seg->prev->next = seg->next;
        else
                sk->u.tcp.ofo = seg->next;
        if (seg->next != NULL)
                seg->next->prev = seg->prev;
        else
                sk->u.tcp.ofo_tail = seg->prev;
        seg->prev = NULL;
        seg->next = NULL;
        sk->u.tcp.ofo_count--;
        sk->u.tcp.ofo_bytes -= seg->len;
        tcp_ofo_metrics_release(seg->len, true);
        if (sk->u.tcp.ofo_count == 0)
                sk->u.tcp.ofo_reorder_distance_peak = 0;
}

static void tcp_ofo_rekey_after_trim(struct nsock *sk, struct tcp_ofo_seg *seg,
                                     uint32_t skip) {
        rb_erase(&seg->rb, &sk->u.tcp.ofo_tree);
        seg->seq += skip;
        seg->data_off += skip;
        seg->len -= skip;
        sk->u.tcp.ofo_bytes -= skip;
        tcp_ofo_metrics_release(skip, false);
        tcp_ofo_rcvbuf_sub(sk, skip);
        tcp_ofo_tree_insert(sk, seg);
}

static void tcp_ofo_seg_free(struct tcp_ofo_seg *seg) {
        struct tcp_owner_memory *memory;

        if (seg == NULL)
                return;
        memory = tcp_ofo_memory_current();
        if (seg->storage != NULL && memory != NULL)
                tcp_memory_payload_free(memory, seg->storage);
        else if (seg->data != NULL)
                rte_free(seg->data);
        if (memory != NULL)
                tcp_memory_ofo_seg_free(memory, seg);
        else
                rte_free(seg);
}

void tcp_ofo_purge(struct nsock *sk) {
        struct tcp_ofo_seg *seg = sk->u.tcp.ofo;

        while (seg != NULL) {
                struct tcp_ofo_seg *next = seg->next;

                tcp_ofo_rcvbuf_sub(sk, seg->len);
                tcp_ofo_unlink(sk, seg);
                tcp_ofo_seg_free(seg);
                seg = next;
        }
        rb_root_init(&sk->u.tcp.ofo_tree);
        sk->u.tcp.ofo = NULL;
        sk->u.tcp.ofo_tail = NULL;
        sk->u.tcp.ofo_count = 0;
        sk->u.tcp.ofo_bytes = 0;
        sk->u.tcp.ofo_reorder_distance_peak = 0;
        sk->u.tcp.sack_recent_valid = false;
        sk->u.tcp.sack_history_count = 0;
        sk->u.tcp.dsack_pending = false;
}

static int tcp_ofo_link(struct nsock *sk, uint32_t seq, const uint8_t *data,
                        uint32_t len, int has_fin,
                        struct tcp_ofo_seg *before) {
        struct tcp_ofo_seg *seg;
        struct tcp_ofo_owner_state *state = tcp_ofo_state_current();
        struct tcp_ofo_limits limits;
        struct tcp_owner_memory *memory;
        uint32_t reorder_distance;
        uint32_t episode_distance;

        if (len == 0 && !has_fin)
                return 0;
        if (before != NULL && before->seq == seq)
                return 0;

        reorder_distance = tcp_ofo_seq_gt(seq, sk->u.tcp.recv_ack)
                               ? seq - sk->u.tcp.recv_ack
                               : 0;
        episode_distance = reorder_distance;
        if (episode_distance < sk->u.tcp.ofo_reorder_distance_peak)
                episode_distance = sk->u.tcp.ofo_reorder_distance_peak;
        if (state != NULL &&
            reorder_distance > state->metrics.reorder_distance_max)
                state->metrics.reorder_distance_max = reorder_distance;
        limits = tcp_ofo_effective_limits(state, episode_distance);

        if (len > tcp_ofo_rcv_wnd(sk)) {
                if (state != NULL)
                        state->metrics.drop_rcvbuf++;
                LOG_WARN("tcp ofo no rcvbuf space " TCP_OFO_ID_FMT
                         ", drop seq=%u len=%u",
                         TCP_OFO_ID_ARG(sk), seq, len);
                return -1;
        }
        if (sk->u.tcp.ofo_count >= limits.segments) {
                if (state != NULL) {
                        state->metrics.drop_seg_limit++;
                        if (limits.segments < TCP_OFO_MAX_SEGS)
                                state->metrics.drop_pressure++;
                }
                LOG_WARN("tcp ofo full " TCP_OFO_ID_FMT
                         ", drop seq=%u len=%u",
                         TCP_OFO_ID_ARG(sk), seq, len);
                return -1;
        }
        if (sk->u.tcp.ofo_bytes > limits.bytes ||
            len > limits.bytes - sk->u.tcp.ofo_bytes) {
                if (state != NULL) {
                        state->metrics.drop_byte_limit++;
                        if (limits.bytes < TCP_OFO_MAX_BYTES)
                                state->metrics.drop_pressure++;
                }
                LOG_WARN("tcp ofo byte cap " TCP_OFO_ID_FMT
                         ", drop seq=%u len=%u",
                         TCP_OFO_ID_ARG(sk), seq, len);
                return -1;
        }
        if (tcp_ofo_owner_can_reserve(state, len) != 0) {
                if (state != NULL)
                        state->metrics.drop_owner_limit++;
                LOG_WARN("tcp ofo owner cap " TCP_OFO_ID_FMT
                         ", drop seq=%u len=%u",
                         TCP_OFO_ID_ARG(sk), seq, len);
                return -1;
        }
        if (len > TCP_MEMORY_CHUNK_SIZE || (len > 0 && data == NULL)) {
                if (state != NULL)
                        state->metrics.drop_alloc++;
                return -1;
        }
#ifdef TCP_TESTING
        if (state != NULL && state->fail_next_alloc) {
                state->fail_next_alloc = false;
                state->metrics.drop_alloc++;
                return -1;
        }
#endif
        memory = tcp_ofo_memory_current();
        seg = memory == NULL ? rte_zmalloc("tcp_ofo_seg", sizeof(*seg), 0)
                             : tcp_memory_ofo_seg_alloc(memory);
        if (seg == NULL) {
                if (state != NULL)
                        state->metrics.drop_alloc++;
                return -1;
        }
        memset(seg, 0, sizeof(*seg));
        rb_node_init(&seg->rb);
        seg->seq = seq;
        seg->len = len;
        seg->has_fin = has_fin ? 1 : 0;
        if (len > 0) {
                if ((memory != NULL &&
                     tcp_memory_payload_alloc(memory, &seg->data,
                                              &seg->storage) != 0) ||
                    (memory == NULL &&
                     (seg->data = rte_malloc("tcp_ofo_data", len, 0)) ==
                         NULL)) {
                        if (state != NULL)
                                state->metrics.drop_alloc++;
                        if (memory != NULL)
                                tcp_memory_ofo_seg_free(memory, seg);
                        else
                                rte_free(seg);
                        return -1;
                }
                rte_memcpy(seg->data, data, len);
        }
        tcp_ofo_tree_insert(sk, seg);
        tcp_ofo_list_insert_before(sk, seg, before);
        sk->u.tcp.ofo_count++;
        sk->u.tcp.ofo_bytes += len;
        sk->u.tcp.ofo_reorder_distance_peak = episode_distance;
        sk->u.tcp.rcvbuf_used += len;
        tcp_ofo_metrics_accept(state, len);
        tcp_ofo_update_pressure(state);
        return 0;
}

int tcp_ofo_queue_insert(struct nsock *sk, uint32_t seq, const uint8_t *data,
                         uint32_t len, int has_fin) {
        uint32_t rcv_nxt = sk->u.tcp.recv_ack;
        uint32_t wnd_end = rcv_nxt + tcp_ofo_rcv_wnd(sk);
        struct tcp_ofo_seg *before;
        struct tcp_ofo_seg *cur;
        struct rb_node *prev_node;

        if (tcp_ofo_seq_lt(seq, rcv_nxt)) {
                uint32_t skip = rcv_nxt - seq;

                if (skip > len || (skip == len && !has_fin))
                        return 0;
                data += skip;
                len -= skip;
                seq = rcv_nxt;
        }
        if (!tcp_ofo_seq_lt(seq, wnd_end)) {
                if (tcp_ofo_range_buffered(sk, seq, len, has_fin))
                        return 0;
                tcp_ofo_record_rcv_window_drop(sk, seq, len, has_fin);
                return 0;
        }
        if (tcp_ofo_seq_gt(seq + len, wnd_end)) {
                len = wnd_end - seq;
                has_fin = 0;
        }
        if (len == 0 && !has_fin)
                return 0;

        before = tcp_ofo_lower_bound(sk, seq);
        if (before != NULL)
                prev_node = rb_prev(&before->rb);
        else
                prev_node = rb_last(&sk->u.tcp.ofo_tree);
        if (prev_node != NULL) {
                struct tcp_ofo_seg *prev =
                    rb_entry(prev_node, struct tcp_ofo_seg, rb);

                cur = tcp_ofo_seq_gt(prev->seq + prev->len, seq) ? prev
                                                                 : before;
        } else {
                cur = before;
        }

        while (cur != NULL && len > 0) {
                uint32_t cur_end = cur->seq + cur->len;
                uint32_t seg_end = seq + len;

                if (tcp_ofo_seq_leq(cur_end, seq)) {
                        cur = cur->next;
                        continue;
                }
                if (tcp_ofo_seq_leq(seg_end, cur->seq))
                        break;
                if (tcp_ofo_seq_lt(seq, cur->seq)) {
                        uint32_t left = cur->seq - seq;
                        uint32_t skip;

                        if (tcp_ofo_link(sk, seq, data, left, 0, cur) != 0)
                                return -1;
                        skip = cur_end - seq;
                        if (skip > len || (skip == len && !has_fin))
                                return 0;
                        data += skip;
                        len -= skip;
                        seq = cur_end;
                        cur = cur->next;
                        continue;
                }
                {
                        uint32_t skip = cur_end - seq;

                        if (skip > len || (skip == len && !has_fin))
                                return 0;
                        data += skip;
                        len -= skip;
                        seq = cur_end;
                        cur = cur->next;
                }
        }
        return tcp_ofo_link(sk, seq, data, len, has_fin, cur);
}

void tcp_ofo_drain(struct nsock *sk, tcp_ofo_deliver_fn deliver,
                   tcp_ofo_eof_fn eof) {
        while (sk->u.tcp.ofo != NULL) {
                struct tcp_ofo_seg *seg = sk->u.tcp.ofo;
                uint32_t len;
                int fin;

                if (tcp_ofo_seq_gt(seg->seq, sk->u.tcp.recv_ack))
                        break;
                if (tcp_ofo_seq_lt(seg->seq, sk->u.tcp.recv_ack)) {
                        uint32_t skip = sk->u.tcp.recv_ack - seg->seq;

                        if (skip > seg->len ||
                            (skip == seg->len && !seg->has_fin)) {
                                tcp_ofo_rcvbuf_sub(sk, seg->len);
                                tcp_ofo_unlink(sk, seg);
                                tcp_ofo_seg_free(seg);
                                continue;
                        }
                        tcp_ofo_rekey_after_trim(sk, seg, skip);
                }
                if (seg->len > 0 &&
                    (deliver == NULL ||
                     deliver(sk, seg->data + seg->data_off, seg->len) != 0))
                        return;

                len = seg->len;
                fin = seg->has_fin;
                tcp_ofo_unlink(sk, seg);
                tcp_ofo_seg_free(seg);
                sk->u.tcp.recv_ack += len;
                if (fin) {
                        sk->u.tcp.recv_ack++;
                        if (eof != NULL)
                                eof(sk);
                        else if (sk->u.tcp.status == TCP_STATUS_ESTABLISHED)
                                sk->u.tcp.status = TCP_STATUS_CLOSE_WAIT;
                        /* No byte after the first stream FIN is deliverable. */
                        tcp_ofo_purge(sk);
                        return;
                }
        }
}

#ifdef TCP_TESTING
void tcp_test_ofo_init(struct nsock *sk, uint32_t rcv_nxt,
                       uint32_t rcvbuf_size) {
        sk->u.tcp.recv_ack = rcv_nxt;
        sk->u.tcp.rcvbuf_size = rcvbuf_size;
        sk->u.tcp.rcvbuf_used = 0;
        sk->u.tcp.peer_eof = false;
        rb_root_init(&sk->u.tcp.ofo_tree);
        sk->u.tcp.ofo = NULL;
        sk->u.tcp.ofo_tail = NULL;
        sk->u.tcp.ofo_count = 0;
        sk->u.tcp.ofo_bytes = 0;
        sk->u.tcp.ofo_reorder_distance_peak = 0;
}

struct tcp_ofo_seg *tcp_test_ofo_lower_bound(const struct nsock *sk,
                                             uint32_t seq) {
        return tcp_ofo_lower_bound(sk, seq);
}

void tcp_test_ofo_purge(struct nsock *sk) { tcp_ofo_purge(sk); }

void tcp_test_ofo_metrics_reset(void) {
        tcp_ofo_metrics_reset_owner(rte_lcore_id());
}

void tcp_test_ofo_force_pressure(bool enabled) {
        struct tcp_ofo_owner_state *state = tcp_ofo_state_current();

        if (state == NULL)
                return;
        state->force_pressure_set = true;
        state->force_pressure = enabled;
        tcp_ofo_update_pressure(state);
}

void tcp_test_ofo_use_auto_pressure(void) {
        struct tcp_ofo_owner_state *state = tcp_ofo_state_current();

        if (state == NULL)
                return;
        state->force_pressure_set = false;
        tcp_ofo_update_pressure(state);
}

void tcp_test_ofo_set_owner_limit(uint64_t bytes) {
        struct tcp_ofo_owner_state *state = tcp_ofo_state_current();

        if (state != NULL)
                state->owner_limit_override = bytes;
}

void tcp_test_ofo_fail_next_alloc(void) {
        struct tcp_ofo_owner_state *state = tcp_ofo_state_current();

        if (state != NULL)
                state->fail_next_alloc = true;
}
#endif
