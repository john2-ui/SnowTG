/**
 * @file tcp_sack.c
 * @brief RFC 6675 SACK scoreboard, Pipe estimator, and NextSeg selection.
 */
#include "tcp_sack.h"

#include "config.h"
#include "tcp.h"
#include "tcp_memory.h"

#ifndef TCP_TESTING
#include "socket_owner_internal.h"
#endif

#include <rte_malloc.h>
#include <string.h>

/** Modulo-2^32 TCP sequence less-than comparison. */
static inline bool seq_lt(uint32_t a, uint32_t b) {
        return (int32_t)(a - b) < 0;
}
/** Modulo-2^32 TCP sequence less-than-or-equal comparison. */
static inline bool seq_leq(uint32_t a, uint32_t b) {
        return (int32_t)(a - b) <= 0;
}
/** Modulo-2^32 TCP sequence greater-than comparison. */
static inline bool seq_gt(uint32_t a, uint32_t b) { return seq_lt(b, a); }
/** Return the earlier of two sequence values in serial-number order. */
static inline uint32_t seq_min(uint32_t a, uint32_t b) {
        return seq_lt(a, b) ? a : b;
}

#ifndef TCP_TESTING
/** Return the owner-local TCP pools used by the current packet worker. */
static struct tcp_owner_memory *sack_memory_current(void) {
        return socket_owner_tcp_memory();
}
#endif

/**
 * @brief Allocate one scoreboard node subject to the per-stream range cap.
 * @return A zeroed owner-local node, or NULL on pool/cap exhaustion.
 */
static struct tcp_sack_range *range_alloc(struct tcp_stream *tp) {
        struct tcp_owner_memory *memory;
        struct tcp_sack_range *range;

        if (tp->sack.range_count >= TCP_SACK_SCORE_MAX_RANGES)
                return NULL;
#ifdef TCP_TESTING
        (void)memory;
        range = rte_zmalloc("tcp_sack_range", sizeof(*range), 0);
#else
        memory = sack_memory_current();
        range = memory == NULL
                    ? rte_zmalloc("tcp_sack_range", sizeof(*range), 0)
                    : tcp_memory_sack_range_alloc(memory);
#endif
        if (range != NULL)
                tp->sack.range_count++;
        return range;
}

/** Return one scoreboard node to its owner and update per-stream accounting. */
static void range_free(struct tcp_stream *tp, struct tcp_sack_range *range) {
        struct tcp_owner_memory *memory;

        if (range == NULL)
                return;
#ifdef TCP_TESTING
        (void)memory;
        rte_free(range);
#else
        memory = sack_memory_current();
        if (memory == NULL)
                rte_free(range);
        else
                tcp_memory_sack_range_free(memory, range);
#endif
        if (tp->sack.range_count != 0)
                tp->sack.range_count--;
}

/** Release every node reachable from @p head. */
static void range_list_clear(struct tcp_stream *tp,
                             struct tcp_sack_range **head) {
        while (*head != NULL) {
                struct tcp_sack_range *next = (*head)->next;

                range_free(tp, *head);
                *head = next;
        }
}

/**
 * @brief Abandon advisory recovery state after a range allocation failure.
 *
 * Unacknowledged sndbuf payload is deliberately untouched.  SACK recovery
 * remains disabled until cumulative ACK reaches the flight's saved HighData,
 * leaving the reliable RTO path as the fallback.
 */
static void sack_degrade(struct tcp_stream *tp) {
        range_list_clear(tp, &tp->sack.sacked);
        range_list_clear(tp, &tp->sack.retransmitted);
        tp->sack.degraded = true;
        tp->sack.degraded_until = tp->sack.high_data;
        tp->sack.mode = TCP_RECOVERY_NORMAL;
        tp->sack.pipe = 0;
        tp->sack.dup_acks = 0;
        tp->sack.limited_transmit = false;
        memset(&tp->sack.pending, 0, sizeof(tp->sack.pending));
}

/**
 * @brief Insert one interval into a sorted, non-overlapping list.
 * @return Newly covered bytes, or UINT32_MAX if a new node cannot be allocated.
 *
 * Adjacent ranges are coalesced.  The first overlapping node is reused and all
 * other merged nodes are immediately returned to the owner-local pool.
 */
static uint32_t range_insert(struct tcp_stream *tp,
                             struct tcp_sack_range **head, uint32_t left,
                             uint32_t right, uint8_t flags) {
        struct tcp_sack_range *prev = NULL;
        struct tcp_sack_range *cur = *head;
        struct tcp_sack_range *reuse = NULL;
        uint32_t newly_covered = right - left;
        uint32_t original_left = left;
        uint32_t original_right = right;

        while (cur != NULL && seq_lt(cur->right, left)) {
                prev = cur;
                cur = cur->next;
        }
        while (cur != NULL && !seq_gt(cur->left, right)) {
                struct tcp_sack_range *next = cur->next;
                uint32_t overlap_left = seq_gt(cur->left, original_left)
                                            ? cur->left
                                            : original_left;
                uint32_t overlap_right = seq_lt(cur->right, original_right)
                                             ? cur->right
                                             : original_right;

                if (seq_lt(overlap_left, overlap_right))
                        newly_covered -= overlap_right - overlap_left;
                if (seq_lt(cur->left, left))
                        left = cur->left;
                if (seq_gt(cur->right, right))
                        right = cur->right;
                if (reuse == NULL) {
                        reuse = cur;
                } else {
                        range_free(tp, cur);
                }
                cur = next;
        }

        if (reuse == NULL) {
                reuse = range_alloc(tp);
                if (reuse == NULL)
                        return UINT32_MAX;
        }
        reuse->left = left;
        reuse->right = right;
        reuse->flags = flags;
        reuse->next = cur;
        if (prev == NULL)
                *head = reuse;
        else
                prev->next = reuse;
        return newly_covered;
}

/** Clip a sorted range list to the half-open interval [left, right). */
static void range_list_trim(struct tcp_stream *tp,
                            struct tcp_sack_range **head, uint32_t left,
                            uint32_t right) {
        struct tcp_sack_range *cur = *head;
        struct tcp_sack_range *prev = NULL;

        while (cur != NULL) {
                struct tcp_sack_range *next = cur->next;

                if (!seq_lt(cur->left, right) || !seq_lt(left, cur->right)) {
                        if (prev == NULL)
                                *head = next;
                        else
                                prev->next = next;
                        range_free(tp, cur);
                } else {
                        if (seq_lt(cur->left, left))
                                cur->left = left;
                        if (seq_gt(cur->right, right))
                                cur->right = right;
                        prev = cur;
                }
                cur = next;
        }
}

/** Return whether [left, right) intersects any range in @p head. */
static bool range_intersects(const struct tcp_sack_range *head,
                             uint32_t left, uint32_t right) {
        for (; head != NULL; head = head->next) {
                if (!seq_lt(head->left, right))
                        break;
                if (seq_lt(left, head->right))
                        return true;
        }
        return false;
}

/** Return whether one serial-number interval fully contains another. */
static bool block_contains(const struct tcp_sack_block *outer,
                           const struct tcp_sack_block *inner) {
        return seq_leq(outer->left, inner->left) &&
               seq_leq(inner->right, outer->right);
}

/** @copydoc tcp_sack_state_init */
void tcp_sack_state_init(struct tcp_stream *tp, uint32_t initial_seq) {
        memset(&tp->sack, 0, sizeof(tp->sack));
        tp->sack.high_data = initial_seq;
        tp->sack.high_rxt = initial_seq;
        tp->sack.rescue_rxt = initial_seq;
        tp->sack.recovery_point = initial_seq;
}

/** @copydoc tcp_sack_state_reset */
void tcp_sack_state_reset(struct tcp_stream *tp, uint32_t initial_seq) {
        range_list_clear(tp, &tp->sack.sacked);
        range_list_clear(tp, &tp->sack.retransmitted);
        tcp_sack_state_init(tp, initial_seq);
}

/** @copydoc tcp_sack_trim */
void tcp_sack_trim(struct tcp_stream *tp, uint32_t flight_end) {
        if (!seq_lt(tp->snd_una, flight_end)) {
                range_list_clear(tp, &tp->sack.sacked);
                range_list_clear(tp, &tp->sack.retransmitted);
        } else {
                range_list_trim(tp, &tp->sack.sacked, tp->snd_una,
                                flight_end);
                range_list_trim(tp, &tp->sack.retransmitted, tp->snd_una,
                                flight_end);
        }
        if (tp->sack.degraded &&
            !seq_lt(tp->snd_una, tp->sack.degraded_until))
                tp->sack.degraded = false;
        if (tp->sack.pending.kind != TCP_RECOVERY_TX_NONE &&
            (!seq_lt(tp->sack.pending.seq, flight_end) ||
             !seq_lt(tp->snd_una, tp->sack.pending.end)))
                memset(&tp->sack.pending, 0, sizeof(tp->sack.pending));
}

/** @copydoc tcp_sack_update */
struct tcp_sack_ack_result
tcp_sack_update(struct tcp_stream *tp, uint32_t packet_ack,
                const struct tcp_sack_block *blocks, uint8_t count,
                uint32_t flight_end) {
        struct tcp_sack_ack_result result;
        bool first_is_dsack = false;

        memset(&result, 0, sizeof(result));
        tcp_sack_trim(tp, flight_end);
        if (count > TCP_SACK_MAX_BLOCKS)
                count = TCP_SACK_MAX_BLOCKS;

        if (count != 0) {
                /* RFC 2883: the first block is D-SACK when it lies entirely
                 * below ACK or when the second block contains it. */
                first_is_dsack = seq_leq(blocks[0].right, packet_ack) ||
                                 (count > 1 &&
                                  block_contains(&blocks[1], &blocks[0]));
                if (first_is_dsack) {
                        result.dsack_valid = true;
                        result.dsack = blocks[0];
                        result.dsack_covers_retransmission = range_intersects(
                            tp->sack.retransmitted, blocks[0].left,
                            blocks[0].right);
                }
        }

        if (tp->sack.degraded || !tp->sack_permitted ||
            !seq_lt(tp->snd_una, flight_end))
                goto recovery_exit;

        for (uint8_t i = first_is_dsack ? 1U : 0U; i < count; i++) {
                uint32_t left = blocks[i].left;
                uint32_t right = blocks[i].right;
                uint32_t added;

                /* FIN occupies sequence space but is not sndbuf payload and
                 * therefore cannot enter this payload-only scoreboard. */
                if (seq_lt(left, tp->snd_una))
                        left = tp->snd_una;
                if (seq_gt(right, flight_end))
                        right = flight_end;
                if (!seq_lt(left, right))
                        continue;
                added = range_insert(tp, &tp->sack.sacked, left, right,
                                     TCP_SACK_RANGE_SACKED);
                if (added == UINT32_MAX) {
                        sack_degrade(tp);
                        result.newly_sacked_bytes = 0;
                        result.new_sack_information = false;
                        goto recovery_exit;
                }
                result.newly_sacked_bytes += added;
        }
        result.new_sack_information = result.newly_sacked_bytes != 0;

recovery_exit:
        /* RecoveryPoint is fixed at entry.  Later new data does not extend it,
         * so a cumulative ACK at the boundary unambiguously ends recovery. */
        if ((tp->sack.mode == TCP_RECOVERY_SACK ||
             tp->sack.mode == TCP_RECOVERY_RTO) &&
            !seq_lt(tp->snd_una, tp->sack.recovery_point)) {
                tp->sack.mode = TCP_RECOVERY_NORMAL;
                tp->sack.pipe = 0;
                tp->sack.dup_acks = 0;
                tp->sack.limited_transmit = false;
                memset(&tp->sack.pending, 0, sizeof(tp->sack.pending));
                result.recovery_exited = true;
        }
        return result;
}

/** @copydoc tcp_sack_is_lost */
bool tcp_sack_is_lost(const struct tcp_stream *tp, uint32_t seq) {
        uint32_t bytes_above = 0;
        uint32_t discontiguous = 0;
        uint32_t smss = tp->snd_mss != 0 ? tp->snd_mss : TCP_DEFAULT_MSS;

        for (const struct tcp_sack_range *r = tp->sack.sacked; r != NULL;
             r = r->next) {
                uint32_t left;

                if (!seq_gt(r->right, seq))
                        continue;
                left = seq_gt(r->left, seq) ? r->left : seq;
                if (seq_lt(left, r->right))
                        bytes_above += r->right - left;
                if (seq_gt(r->left, seq))
                        discontiguous++;
                if (discontiguous >= TCP_SACK_DUP_THRESH ||
                    bytes_above > (TCP_SACK_DUP_THRESH - 1U) * smss)
                        return true;
        }
        return false;
}

/** Count retransmitted bytes below HighRxt that overlap [left, right). */
static uint32_t retransmitted_overlap(const struct tcp_stream *tp,
                                      uint32_t left, uint32_t right) {
        uint32_t bytes = 0;

        for (const struct tcp_sack_range *r = tp->sack.retransmitted;
             r != NULL; r = r->next) {
                uint32_t l;
                uint32_t rr;

                if (!seq_lt(r->left, right))
                        break;
                if (!seq_lt(left, r->right))
                        continue;
                l = seq_gt(left, r->left) ? left : r->left;
                rr = seq_lt(right, r->right) ? right : r->right;
                if (seq_lt(l, rr) && seq_lt(l, tp->sack.high_rxt)) {
                        if (seq_gt(rr, tp->sack.high_rxt))
                                rr = tp->sack.high_rxt;
                        bytes += rr - l;
                }
        }
        return bytes;
}

/**
 * @brief Compute the RFC 6675 Pipe contribution of one unsacked interval.
 *
 * Retransmitted bytes below HighRxt remain in the network estimate.  Original
 * bytes count only while IsLost does not declare them lost.
 */
static uint32_t pipe_for_unsacked(struct tcp_stream *tp, uint32_t left,
                                  uint32_t right) {
        uint32_t pipe = retransmitted_overlap(tp, left, right);

        if (!seq_lt(left, right))
                return pipe;
        if (!tcp_sack_is_lost(tp, left))
                return pipe + (right - left);

        /* IsLost is monotonic inside one unsacked interval. Find first byte
         * no longer considered lost without walking every byte. */
        uint32_t lo = 0;
        uint32_t hi = right - left;
        while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2U;

                if (tcp_sack_is_lost(tp, left + mid))
                        lo = mid + 1U;
                else
                        hi = mid;
        }
        return pipe + (right - (left + lo));
}

/** @copydoc tcp_sack_set_pipe */
uint32_t tcp_sack_set_pipe(struct tcp_stream *tp, uint32_t flight_end) {
        uint32_t cursor = tp->snd_una;
        uint32_t pipe = 0;

        for (const struct tcp_sack_range *r = tp->sack.sacked;
             r != NULL && seq_lt(cursor, flight_end); r = r->next) {
                if (seq_lt(cursor, r->left))
                        pipe += pipe_for_unsacked(tp, cursor,
                                                 seq_min(r->left, flight_end));
                if (seq_gt(r->right, cursor))
                        cursor = r->right;
        }
        if (seq_lt(cursor, flight_end))
                pipe += pipe_for_unsacked(tp, cursor, flight_end);
        tp->sack.pipe = pipe;
        return pipe;
}

/** Save one SMSS-limited NextSeg choice without committing sender state. */
static bool candidate_set(struct tcp_stream *tp,
                          enum tcp_recovery_tx_kind kind, uint8_t rule,
                          bool rescue, uint32_t seq, uint32_t end) {
        uint32_t smss = tp->snd_mss != 0 ? tp->snd_mss : TCP_DEFAULT_MSS;

        if (!seq_lt(seq, end))
                return false;
        if (end - seq > smss)
                end = seq + smss;
        tp->sack.pending.kind = kind;
        tp->sack.pending.nextseg_rule = rule;
        tp->sack.pending.rescue = rescue;
        tp->sack.pending.seq = seq;
        tp->sack.pending.end = end;
        return true;
}

/** Return the lowest unsacked subrange of [left, right). */
static bool find_unsacked(const struct tcp_stream *tp, uint32_t left,
                          uint32_t right, uint32_t *out_left,
                          uint32_t *out_right) {
        uint32_t cursor = left;

        for (const struct tcp_sack_range *r = tp->sack.sacked;
             r != NULL && seq_lt(cursor, right); r = r->next) {
                if (!seq_lt(r->right, cursor) && seq_lt(cursor, r->left)) {
                        *out_left = cursor;
                        *out_right = seq_min(r->left, right);
                        return true;
                }
                if (seq_gt(r->right, cursor))
                        cursor = r->right;
        }
        if (seq_lt(cursor, right)) {
                *out_left = cursor;
                *out_right = right;
                return true;
        }
        return false;
}

/** Return the greatest right edge currently present in the SACKED list. */
static uint32_t highest_sacked(const struct tcp_stream *tp) {
        const struct tcp_sack_range *r = tp->sack.sacked;
        uint32_t highest = tp->snd_una;

        for (; r != NULL; r = r->next)
                if (seq_gt(r->right, highest))
                        highest = r->right;
        return highest;
}

/** Return the highest unsacked subrange of [left, right). */
static bool find_highest_unsacked(const struct tcp_stream *tp, uint32_t left,
                                  uint32_t right, uint32_t *out_left,
                                  uint32_t *out_right) {
        uint32_t cursor = left;
        bool found = false;

        for (const struct tcp_sack_range *r = tp->sack.sacked;
             r != NULL && seq_lt(cursor, right); r = r->next) {
                if (seq_lt(cursor, r->left)) {
                        *out_left = cursor;
                        *out_right = seq_min(r->left, right);
                        found = true;
                }
                if (seq_gt(r->right, cursor))
                        cursor = r->right;
        }
        if (seq_lt(cursor, right)) {
                *out_left = cursor;
                *out_right = right;
                found = true;
        }
        return found;
}

/** @copydoc tcp_sack_enter_recovery */
void tcp_sack_enter_recovery(struct tcp_stream *tp,
                             enum tcp_recovery_mode mode,
                             uint32_t flight_end) {
        uint32_t end = seq_min(tp->snd_una +
                                   (tp->snd_mss != 0 ? tp->snd_mss
                                                     : TCP_DEFAULT_MSS),
                               flight_end);

        tp->sack.mode = mode;
        tp->sack.recovery_point = flight_end;
        tp->sack.high_rxt = tp->snd_una;
        tp->sack.rescue_rxt = tp->snd_una;
        tp->sack.limited_transmit = false;
        (void)candidate_set(tp, TCP_RECOVERY_TX_RETRANSMIT, 1, false,
                            tp->snd_una, end);
}

/** @copydoc tcp_sack_schedule_newreno_partial */
bool tcp_sack_schedule_newreno_partial(struct tcp_stream *tp,
                                       uint32_t flight_end) {
        uint32_t smss = tp->snd_mss != 0 ? tp->snd_mss : TCP_DEFAULT_MSS;
        uint32_t end;

        if (tp->sack.mode != TCP_RECOVERY_NEWRENO ||
            !seq_lt(tp->snd_una, flight_end))
                return false;
        end = seq_min(tp->snd_una + smss, flight_end);
        memset(&tp->sack.pending, 0, sizeof(tp->sack.pending));
        return candidate_set(tp, TCP_RECOVERY_TX_RETRANSMIT, 1, false,
                             tp->snd_una, end);
}

/** @copydoc tcp_sack_on_rto */
void tcp_sack_on_rto(struct tcp_stream *tp, uint32_t flight_end) {
        range_list_clear(tp, &tp->sack.sacked);
        tp->sack.degraded = false;
        tp->sack.dup_acks = 0;
        tcp_sack_enter_recovery(tp, TCP_RECOVERY_RTO, flight_end);
}

/** @copydoc tcp_sack_schedule_next */
bool tcp_sack_schedule_next(struct tcp_stream *tp, uint32_t sndbuf_end,
                            bool allow_new_data) {
        uint32_t left;
        uint32_t right;
        uint32_t highest;

        if (tp->sack.pending.kind != TCP_RECOVERY_TX_NONE)
                return true;
        if (tp->sack.degraded)
                return false;

        if (tp->sack.mode == TCP_RECOVERY_RTO) {
                /* RTO recovery always fills the lowest currently unsacked hole
                 * and never enables normal fast-recovery NextSeg scheduling. */
                if (find_unsacked(tp, tp->snd_una, tp->sack.high_data, &left,
                                  &right))
                        return candidate_set(tp, TCP_RECOVERY_TX_RETRANSMIT,
                                             1, false, left, right);
                return false;
        }
        if (tp->sack.mode != TCP_RECOVERY_SACK)
                return false;

        highest = highest_sacked(tp);
        left = seq_gt(tp->sack.high_rxt, tp->snd_una) ? tp->sack.high_rxt
                                                      : tp->snd_una;
        /* NextSeg rule 1: lowest unsacked range that IsLost declares lost. */
        while (find_unsacked(tp, left, highest, &left, &right)) {
                if (tcp_sack_is_lost(tp, left))
                        return candidate_set(tp, TCP_RECOVERY_TX_RETRANSMIT,
                                             1, false, left, right);
                left = right;
        }

        /* NextSeg rule 2: transmit previously unsent payload when allowed. */
        if (allow_new_data && seq_lt(tp->sack.high_data, sndbuf_end))
                return candidate_set(tp, TCP_RECOVERY_TX_NEW_DATA, 2, false,
                                     tp->sack.high_data, sndbuf_end);

        left = seq_gt(tp->sack.high_rxt, tp->snd_una) ? tp->sack.high_rxt
                                                      : tp->snd_una;
        /* NextSeg rule 3: infer loss below the highest SACKed sequence. */
        if (find_unsacked(tp, left, highest, &left, &right))
                return candidate_set(tp, TCP_RECOVERY_TX_RETRANSMIT, 3,
                                     false, left, right);

        /* NextSeg rule 4: at most one highest-possible rescue retransmission
         * before RecoveryPoint is cumulatively acknowledged. */
        if (!seq_lt(tp->snd_una, tp->sack.rescue_rxt) &&
            find_highest_unsacked(tp, tp->snd_una, tp->sack.high_data, &left,
                                  &right)) {
                uint32_t smss = tp->snd_mss != 0 ? tp->snd_mss
                                                 : TCP_DEFAULT_MSS;
                uint32_t start = right - left > smss ? right - smss : left;

                return candidate_set(tp, TCP_RECOVERY_TX_RETRANSMIT, 4, true,
                                     start, right);
        }
        return false;
}

/**
 * @copydoc tcp_sack_commit_candidate
 *
 * This function is intentionally called only after TX-ring enqueue succeeds;
 * transient send failures must leave HighRxt, Pipe, and rescue guards intact.
 */
void tcp_sack_commit_candidate(struct tcp_stream *tp, uint32_t sent_end) {
        struct tcp_recovery_candidate candidate = tp->sack.pending;

        if (candidate.kind == TCP_RECOVERY_TX_NONE ||
            !seq_lt(candidate.seq, sent_end))
                return;
        if (candidate.kind == TCP_RECOVERY_TX_NEW_DATA) {
                if (seq_gt(sent_end, tp->sack.high_data))
                        tp->sack.high_data = sent_end;
        } else {
                if (range_insert(tp, &tp->sack.retransmitted, candidate.seq,
                                 sent_end,
                                 TCP_SACK_RANGE_RETRANSMITTED) == UINT32_MAX) {
                        sack_degrade(tp);
                        return;
                }
                if (candidate.rescue)
                        tp->sack.rescue_rxt = tp->sack.recovery_point;
                else if (seq_gt(sent_end, tp->sack.high_rxt))
                        tp->sack.high_rxt = sent_end;
        }
        if (tp->sack.mode == TCP_RECOVERY_SACK)
                tp->sack.pipe += sent_end - candidate.seq;
        memset(&tp->sack.pending, 0, sizeof(tp->sack.pending));
}

/** @copydoc tcp_sack_cancel_candidate */
void tcp_sack_cancel_candidate(struct tcp_stream *tp) {
        memset(&tp->sack.pending, 0, sizeof(tp->sack.pending));
}

/** @copydoc tcp_sack_note_new_data */
void tcp_sack_note_new_data(struct tcp_stream *tp, uint32_t end_seq) {
        if (seq_gt(end_seq, tp->sack.high_data))
                tp->sack.high_data = end_seq;
}

/** @copydoc tcp_sack_score_count */
uint16_t tcp_sack_score_count(const struct tcp_stream *tp) {
        uint16_t count = 0;

        for (const struct tcp_sack_range *r = tp->sack.sacked; r != NULL;
             r = r->next)
                count++;
        return count;
}
