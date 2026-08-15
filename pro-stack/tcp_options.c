/**
 * @file tcp_options.c
 * @brief TCP option wire encoding, parsing, and per-stream negotiation.
 */
#include "tcp_options.h"

#include "config.h"
#include "socket.h"

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_memcpy.h>

#include <string.h>

#define TCP_OPT_EOL 0
#define TCP_OPT_NOP 1
#define TCP_OPT_MSS 2
#define TCP_OPT_WSCALE 3
#define TCP_OPT_SACK_PERMITTED 4
#define TCP_OPT_SACK 5
#define TCP_OPT_TIMESTAMP 8

#define TCP_OPT_MSS_LEN 4
#define TCP_OPT_WSCALE_LEN 3
#define TCP_OPT_SACK_PERMITTED_LEN 2
#define TCP_OPT_SACK_BASE_LEN 2
#define TCP_OPT_SACK_BLOCK_LEN 8
#define TCP_OPT_TIMESTAMP_LEN 10
#define TCP_OPT_TIMESTAMP_PADDED_LEN 12

#define TCP_MIN_MSS 536
/* RFC 7323 §4.3.3: invalidate TS.Recent after 24 idle days. */
#define TCP_PAWS_IDLE_MS (24U * 24U * 60U * 60U * 1000U)

/** @copydoc tcp_options_now_ms */
uint32_t tcp_options_now_ms(void) {
        uint64_t cycles = rte_get_timer_cycles();

        return (uint32_t)(cycles * 1000 / rte_get_timer_hz());
}

/** Return and remember the local Timestamp value for one emitted segment. */
static uint32_t tcp_options_ts_now(struct nsock *sk) {
        uint32_t now = tcp_options_now_ms();

        sk->u.tcp.ts_last_val = now;
        return now;
}

/** @brief Modulo-2^32 sequence comparison used by the PAWS left-edge test. */
static bool tcp_options_seq_at_or_after(uint32_t seq, uint32_t boundary) {
        return (int32_t)(seq - boundary) >= 0;
}

/** Modulo-2^32 strict sequence comparison used by SACK interval logic. */
static bool tcp_options_seq_after(uint32_t seq, uint32_t boundary) {
        return (int32_t)(seq - boundary) > 0;
}

/**
 * @brief Append raw option bytes and round the fragment length to TCP words.
 * @return 0 on success, or -1 if the 40-byte TCP option area would overflow.
 */
static int tcp_options_append(struct tcp_fragment *f, const void *data,
                              size_t len) {
        size_t offset = (size_t)f->opt_len * sizeof(uint32_t);

        if (offset + len > sizeof(f->options))
                return -1;

        memcpy((uint8_t *)f->options + offset, data, len);
        f->opt_len =
            (int)((offset + len + sizeof(uint32_t) - 1) / sizeof(uint32_t));
        return 0;
}

/** Append a network-order MSS option. */
static int tcp_options_emit_mss(struct tcp_fragment *f, uint16_t mss) {
        const uint8_t opt[TCP_OPT_MSS_LEN] = {
            TCP_OPT_MSS,
            TCP_OPT_MSS_LEN,
            (uint8_t)(mss >> 8),
            (uint8_t)mss,
        };

        return tcp_options_append(f, opt, sizeof(opt));
}

/** Append one NOP-padded Window Scale option. */
static int tcp_options_emit_wscale(struct tcp_fragment *f, uint8_t wscale) {
        const uint8_t opt[4] = {
            TCP_OPT_NOP,
            TCP_OPT_WSCALE,
            TCP_OPT_WSCALE_LEN,
            wscale,
        };

        return tcp_options_append(f, opt, sizeof(opt));
}

/** Append the standard two-NOP-padded Timestamp option. */
static int tcp_options_emit_timestamp(struct tcp_fragment *f, uint32_t tsval,
                                      uint32_t tsecr) {
        uint8_t opt[TCP_OPT_TIMESTAMP_PADDED_LEN] = {
            TCP_OPT_NOP,
            TCP_OPT_NOP,
            TCP_OPT_TIMESTAMP,
            TCP_OPT_TIMESTAMP_LEN,
        };
        uint32_t value;

        value = rte_cpu_to_be_32(tsval);
        rte_memcpy(opt + 4, &value, sizeof(value));
        value = rte_cpu_to_be_32(tsecr);
        rte_memcpy(opt + 8, &value, sizeof(value));
        return tcp_options_append(f, opt, sizeof(opt));
}

/** Append the two-byte RFC 2018 SACK-Permitted option. */
static int tcp_options_emit_sack_permitted(struct tcp_fragment *f) {
        const uint8_t opt[TCP_OPT_SACK_PERMITTED_LEN] = {
            TCP_OPT_SACK_PERMITTED,
            TCP_OPT_SACK_PERMITTED_LEN,
        };

        return tcp_options_append(f, opt, sizeof(opt));
}

/** Return whether @p outer fully contains @p inner in serial-number order. */
static bool tcp_options_block_contains(const struct tcp_sack_block *outer,
                                       const struct tcp_sack_block *inner) {
        return !tcp_options_seq_after(outer->left, inner->left) &&
               !tcp_options_seq_after(inner->right, outer->right);
}

/**
 * @brief Append a block unless an emitted block already contains it.
 * @return True only when @p block was appended within @p capacity.
 */
static bool tcp_options_add_distinct(struct tcp_sack_block *out,
                                     uint8_t *count, uint8_t capacity,
                                     struct tcp_sack_block block) {
        for (uint8_t i = 0; i < *count; i++)
                if (tcp_options_block_contains(&out[i], &block))
                        return false;
        if (*count >= capacity)
                return false;
        out[(*count)++] = block;
        return true;
}

/** Move a successfully reported first block to the front of receiver history. */
static void tcp_options_history_push(struct nsock *sk,
                                     struct tcp_sack_block block) {
        uint8_t existing = TCP_SACK_MAX_BLOCKS;

        for (uint8_t i = 0; i < sk->u.tcp.sack_history_count; i++) {
                if (tcp_options_block_contains(&sk->u.tcp.sack_history[i],
                                               &block) ||
                    tcp_options_block_contains(&block,
                                               &sk->u.tcp.sack_history[i])) {
                        existing = i;
                        break;
                }
        }
        if (existing < sk->u.tcp.sack_history_count) {
                for (uint8_t i = existing; i > 0; i--)
                        sk->u.tcp.sack_history[i] =
                            sk->u.tcp.sack_history[i - 1];
        } else {
                uint8_t count = sk->u.tcp.sack_history_count;
                if (count < TCP_SACK_MAX_BLOCKS)
                        count++;
                for (uint8_t i = count - 1; i > 0; i--)
                        sk->u.tcp.sack_history[i] =
                            sk->u.tcp.sack_history[i - 1];
                sk->u.tcp.sack_history_count = count;
        }
        sk->u.tcp.sack_history[0] = block;
}

/**
 * @brief Build canonical receiver SACK blocks from actual OFO contents.
 *
 * The one-shot D-SACK is emitted first.  Otherwise the current triggering
 * block wins, followed by revalidated recent history and then remaining OFO
 * ranges.  Adjacent/overlapping OFO nodes are merged before ordering.
 * @return Number of host-order blocks written to @p out.
 */
static uint8_t tcp_options_collect_sacks(struct nsock *sk,
                                         struct tcp_sack_block *out,
                                         uint8_t capacity) {
        struct tcp_sack_block normalized[TCP_OFO_MAX_SEGS];
        uint8_t count = 0;
        int recent = -1;

        for (const struct tcp_ofo_seg *seg = sk->u.tcp.ofo; seg != NULL;
             seg = seg->next) {
                uint32_t left = seg->seq;
                uint32_t right = seg->seq + seg->len;

                if (seg->len == 0 ||
                    !tcp_options_seq_after(right, sk->u.tcp.recv_ack))
                        continue;
                if (tcp_options_seq_after(sk->u.tcp.recv_ack, left))
                        left = sk->u.tcp.recv_ack;

                if (count != 0 &&
                    !tcp_options_seq_after(left, normalized[count - 1].right)) {
                        if (tcp_options_seq_after(right,
                                                  normalized[count - 1].right))
                                normalized[count - 1].right = right;
                        continue;
                }
                if (count == TCP_OFO_MAX_SEGS)
                        break;
                normalized[count].left = left;
                normalized[count].right = right;
                count++;
        }

        if (sk->u.tcp.sack_recent_valid) {
                for (uint8_t i = 0; i < count; i++) {
                        if (tcp_options_seq_after(normalized[i].right,
                                                  sk->u.tcp.sack_recent.left) &&
                            tcp_options_seq_after(sk->u.tcp.sack_recent.right,
                                                  normalized[i].left)) {
                                recent = i;
                                break;
                        }
                }
                if (recent < 0)
                        sk->u.tcp.sack_recent_valid = false;
        }

        uint8_t emitted = 0;
        if (sk->u.tcp.dsack_pending && capacity != 0) {
                (void)tcp_options_add_distinct(out, &emitted, capacity,
                                               sk->u.tcp.dsack_block);
                /* A duplicate above ACK is followed by the larger OFO block
                 * containing it, as required by RFC 2883. */
                if (tcp_options_seq_after(sk->u.tcp.dsack_block.right,
                                          sk->u.tcp.recv_ack)) {
                        for (uint8_t i = 0; i < count; i++) {
                                if (tcp_options_block_contains(
                                        &normalized[i],
                                        &sk->u.tcp.dsack_block)) {
                                        (void)tcp_options_add_distinct(
                                            out, &emitted, capacity,
                                            normalized[i]);
                                        break;
                                }
                        }
                }
                sk->u.tcp.dsack_pending = false;
        }

        if (recent >= 0) {
                tcp_options_history_push(sk, normalized[recent]);
                (void)tcp_options_add_distinct(out, &emitted, capacity,
                                               normalized[recent]);
        }
        sk->u.tcp.sack_recent_valid = false;

        for (uint8_t h = 0; h < sk->u.tcp.sack_history_count &&
                            emitted < capacity;
             h++) {
                for (uint8_t i = 0; i < count; i++) {
                        if (tcp_options_block_contains(
                                &normalized[i],
                                &sk->u.tcp.sack_history[h])) {
                                (void)tcp_options_add_distinct(
                                    out, &emitted, capacity, normalized[i]);
                                break;
                        }
                }
        }
        for (uint8_t i = 0; i < count && emitted < capacity; i++) {
                (void)tcp_options_add_distinct(out, &emitted, capacity,
                                               normalized[i]);
        }
        return emitted;
}

/**
 * @brief Encode one RFC 2018/2883 SACK option when the fragment permits it.
 * @return 0 when omitted or encoded successfully, or -1 on option overflow.
 */
static int tcp_options_emit_sack(struct nsock *sk, struct tcp_fragment *f) {
        const size_t offset = (size_t)f->opt_len * sizeof(uint32_t);
        uint8_t capacity;
        struct tcp_sack_block blocks[TCP_SACK_MAX_BLOCKS];
        uint8_t count;
        uint8_t opt[TCP_OPT_SACK_BASE_LEN +
                    TCP_SACK_MAX_BLOCKS * TCP_OPT_SACK_BLOCK_LEN] = {0};

        if (!(f->tcp_flags & RTE_TCP_ACK_FLAG) || !sk->u.tcp.sack_permitted ||
            (!sk->u.tcp.dsack_pending && sk->u.tcp.ofo == NULL) ||
            offset + TCP_OPT_SACK_BASE_LEN > 40)
                return 0;

        capacity = (uint8_t)((40 - offset - TCP_OPT_SACK_BASE_LEN) /
                             TCP_OPT_SACK_BLOCK_LEN);
        if (capacity > TCP_SACK_MAX_BLOCKS)
                capacity = TCP_SACK_MAX_BLOCKS;
        count = tcp_options_collect_sacks(sk, blocks, capacity);
        if (count == 0)
                return 0;

        opt[0] = TCP_OPT_SACK;
        opt[1] = (uint8_t)(TCP_OPT_SACK_BASE_LEN +
                           count * TCP_OPT_SACK_BLOCK_LEN);
        for (uint8_t i = 0; i < count; i++) {
                uint32_t value = rte_cpu_to_be_32(blocks[i].left);

                rte_memcpy(opt + TCP_OPT_SACK_BASE_LEN +
                               i * TCP_OPT_SACK_BLOCK_LEN,
                           &value, sizeof(value));
                value = rte_cpu_to_be_32(blocks[i].right);
                rte_memcpy(opt + TCP_OPT_SACK_BASE_LEN +
                               i * TCP_OPT_SACK_BLOCK_LEN + sizeof(value),
                           &value, sizeof(value));
        }
        return tcp_options_append(f, opt, opt[1]);
}

/** Commit the fragment's word-aligned option length to TCP data offset. */
static void tcp_options_finish(struct tcp_fragment *f) {
        f->data_off = (uint8_t)((5 + f->opt_len) << 4);
}

/** @copydoc tcp_options_reset_state */
void tcp_options_reset_state(struct nsock *sk) {
        sk->u.tcp.timestamps_ok = false;
        sk->u.tcp.ts_recent = 0;
        sk->u.tcp.ts_recent_valid = false;
        sk->u.tcp.ts_recent_age_ms = 0;
        sk->u.tcp.ts_last_val = 0;
        sk->u.tcp.rx_timestamp_present = false;
        sk->u.tcp.rx_tsval = 0;
        sk->u.tcp.rx_tsecr = 0;
        sk->u.tcp.sack_local_offered = false;
        sk->u.tcp.sack_peer_permitted = false;
        sk->u.tcp.sack_permitted = false;
        sk->u.tcp.sack_recent_valid = false;
        sk->u.tcp.sack_history_count = 0;
        sk->u.tcp.dsack_pending = false;
        sk->u.tcp.rx_sack_count = 0;
        sk->u.tcp.fin_deferred = false;
        tcp_sack_state_reset(&sk->u.tcp, sk->u.tcp.snd_una);
}

/** @copydoc tcp_options_parse */
int tcp_options_parse(const struct rte_tcp_hdr *hdr,
                      struct tcp_options_rx *rx) {
        const uint8_t hdr_len = (hdr->data_off >> 4) * 4;
        const uint8_t *opt;
        size_t remain;
        bool mss_present = false;
        bool sack_permitted_seen = false;
        bool sack_seen = false;

        if (hdr_len < sizeof(*hdr))
                return -1;

        memset(rx, 0, sizeof(*rx));
        rx->mss = TCP_DEFAULT_MSS;
        opt = (const uint8_t *)hdr + sizeof(*hdr);
        remain = hdr_len - sizeof(*hdr);

        while (remain > 0) {
                uint8_t kind = opt[0];
                uint8_t len;

                if (kind == TCP_OPT_EOL)
                        break;
                if (kind == TCP_OPT_NOP) {
                        opt++;
                        remain--;
                        continue;
                }
                if (remain < 2)
                        return -1;

                len = opt[1];
                if (len < 2 || len > remain)
                        return -1;

                switch (kind) {
                case TCP_OPT_MSS:
                        if (len != TCP_OPT_MSS_LEN || mss_present)
                                return -1;
                        mss_present = true;
                        {
                                uint16_t value =
                                    ((uint16_t)opt[2] << 8) | opt[3];
                                if (value >= TCP_MIN_MSS)
                                        rx->mss = value;
                        }
                        break;
                case TCP_OPT_WSCALE:
                        if (len != TCP_OPT_WSCALE_LEN || rx->wscale_present ||
                            opt[2] > TCP_WSCALE_MAX)
                                return -1;
                        rx->wscale = opt[2];
                        rx->wscale_present = true;
                        break;
                case TCP_OPT_SACK_PERMITTED:
                        if (len != TCP_OPT_SACK_PERMITTED_LEN ||
                            sack_permitted_seen)
                                return -1;
                        sack_permitted_seen = true;
                        if (hdr->tcp_flags & RTE_TCP_SYN_FLAG) {
                                rx->sack_permitted_present = true;
                        }
                        break;
                case TCP_OPT_SACK: {
                        uint8_t count;

                        if (len < TCP_OPT_SACK_BASE_LEN +
                                      TCP_OPT_SACK_BLOCK_LEN ||
                            (len - TCP_OPT_SACK_BASE_LEN) %
                                    TCP_OPT_SACK_BLOCK_LEN !=
                                0 ||
                            sack_seen)
                                return -1;
                        sack_seen = true;
                        count = (uint8_t)((len - TCP_OPT_SACK_BASE_LEN) /
                                          TCP_OPT_SACK_BLOCK_LEN);
                        if (count > TCP_SACK_MAX_BLOCKS)
                                return -1;
                        for (uint8_t i = 0; i < count; i++) {
                                uint32_t left;
                                uint32_t right;

                                rte_memcpy(&left,
                                           opt + TCP_OPT_SACK_BASE_LEN +
                                               i * TCP_OPT_SACK_BLOCK_LEN,
                                           sizeof(left));
                                rte_memcpy(&right,
                                           opt + TCP_OPT_SACK_BASE_LEN +
                                               i * TCP_OPT_SACK_BLOCK_LEN +
                                               sizeof(left),
                                           sizeof(right));
                                left = rte_be_to_cpu_32(left);
                                right = rte_be_to_cpu_32(right);
                                if (!tcp_options_seq_after(right, left))
                                        return -1;
                                if (!(hdr->tcp_flags & RTE_TCP_SYN_FLAG) &&
                                    (hdr->tcp_flags & RTE_TCP_ACK_FLAG)) {
                                        rx->sacks[i].left = left;
                                        rx->sacks[i].right = right;
                                }
                        }
                        if (!(hdr->tcp_flags & RTE_TCP_SYN_FLAG) &&
                            (hdr->tcp_flags & RTE_TCP_ACK_FLAG))
                                rx->sack_count = count;
                        break;
                }
                case TCP_OPT_TIMESTAMP:
                        if (len != TCP_OPT_TIMESTAMP_LEN ||
                            rx->timestamp_present)
                                return -1;
                        {
                                uint32_t tsval;
                                uint32_t tsecr;

                                rte_memcpy(&tsval, opt + 2, sizeof(tsval));
                                rte_memcpy(&tsecr, opt + 6, sizeof(tsecr));
                                rx->tsval = rte_be_to_cpu_32(tsval);
                                rx->tsecr = rte_be_to_cpu_32(tsecr);
                                rx->timestamp_present = true;
                        }
                        break;
                default:
                        break;
                }

                opt += len;
                remain -= len;
        }
        return 0;
}

/** @copydoc tcp_options_negotiate_syn */
void tcp_options_negotiate_syn(struct nsock *sk,
                               const struct tcp_options_rx *peer) {
        sk->u.tcp.snd_mss =
            peer->mss < TCP_DEFAULT_MSS ? peer->mss : TCP_DEFAULT_MSS;
        sk->u.tcp.snd_wscale = peer->wscale;
        sk->u.tcp.wscale_ok = peer->wscale_present;
        sk->u.tcp.timestamps_ok = peer->timestamp_present;
        if (peer->sack_permitted_present)
                sk->u.tcp.sack_peer_permitted = true;
        sk->u.tcp.sack_permitted =
            sk->u.tcp.sack_local_offered && sk->u.tcp.sack_peer_permitted;
        if (peer->timestamp_present) {
                sk->u.tcp.ts_recent = peer->tsval;
                sk->u.tcp.ts_recent_valid = true;
                sk->u.tcp.ts_recent_age_ms = tcp_options_now_ms();
        }
}

/** @copydoc tcp_options_apply_syn */
int tcp_options_apply_syn(struct nsock *sk, struct tcp_fragment *f,
                          bool include_wscale, bool include_timestamp,
                          bool include_sack_permitted, uint32_t tsecr) {
        memset(f->options, 0, sizeof(f->options));
        f->opt_len = 0;

        if (tcp_options_emit_mss(f, TCP_DEFAULT_MSS) != 0 ||
            (include_wscale &&
             tcp_options_emit_wscale(f, sk->u.tcp.rcv_wscale) != 0) ||
            (include_timestamp &&
             tcp_options_emit_timestamp(f, tcp_options_ts_now(sk), tsecr) !=
                 0) ||
            (include_sack_permitted &&
             tcp_options_emit_sack_permitted(f) != 0))
                return -1;

        sk->u.tcp.sack_local_offered = include_sack_permitted;
        sk->u.tcp.sack_permitted =
            sk->u.tcp.sack_local_offered && sk->u.tcp.sack_peer_permitted;
        tcp_options_finish(f);
        return 0;
}

/** @copydoc tcp_options_apply_established */
int tcp_options_apply_established(struct nsock *sk, struct tcp_fragment *f) {
        memset(f->options, 0, sizeof(f->options));
        f->opt_len = 0;

        if (sk->u.tcp.timestamps_ok &&
            tcp_options_emit_timestamp(f, tcp_options_ts_now(sk),
                                       sk->u.tcp.ts_recent) != 0)
                return -1;
        if (tcp_options_emit_sack(sk, f) != 0)
                return -1;

        tcp_options_finish(f);
        return 0;
}

/** @copydoc tcp_options_process_inbound */
int tcp_options_process_inbound(struct nsock *sk,
                                const struct tcp_options_rx *rx, bool is_rst,
                                uint32_t seg_seq, bool seq_acceptable) {
        sk->u.tcp.rx_timestamp_present = rx->timestamp_present;
        sk->u.tcp.rx_tsval = rx->tsval;
        sk->u.tcp.rx_tsecr = rx->tsecr;
        sk->u.tcp.rx_sack_count = rx->sack_count;
        if (rx->sack_count != 0)
                rte_memcpy(sk->u.tcp.rx_sacks, rx->sacks,
                           rx->sack_count * sizeof(rx->sacks[0]));

        if (is_rst || !sk->u.tcp.timestamps_ok)
                return 0;
        if (!rx->timestamp_present)
                return -1;

        /*
         * RFC 7323 §5.2 PAWS.  The signed subtraction is a serial comparison
         * in the modulo-2^32 Timestamp space.  A segment that starts left of
         * RCV.NXT is a duplicate/overlap and keeps the RFC left-edge exception.
         * Its Timestamp must therefore not cause a PAWS drop.
         */
        if (sk->u.tcp.ts_recent_valid &&
            (uint32_t)(tcp_options_now_ms() - sk->u.tcp.ts_recent_age_ms) >
                TCP_PAWS_IDLE_MS)
                sk->u.tcp.ts_recent_valid = false;

        if (seq_acceptable &&
            tcp_options_seq_at_or_after(seg_seq, sk->u.tcp.recv_ack) &&
            sk->u.tcp.ts_recent_valid &&
            (int32_t)(rx->tsval - sk->u.tcp.ts_recent) < 0)
                return -2;

        /*
         * TS.Recent follows accepted in-order receive progress only.  It is
         * committed by tcp_options_note_receive_progress() after the state
         * handler has actually advanced recv_ack.
         */
        return 0;
}

/** @copydoc tcp_options_note_receive_progress */
void tcp_options_note_receive_progress(struct nsock *sk) {
        if (!sk->u.tcp.timestamps_ok || !sk->u.tcp.rx_timestamp_present)
                return;

        sk->u.tcp.ts_recent = sk->u.tcp.rx_tsval;
        sk->u.tcp.ts_recent_valid = true;
        sk->u.tcp.ts_recent_age_ms = tcp_options_now_ms();
}

/** @copydoc tcp_options_data_mss */
uint16_t tcp_options_data_mss(const struct nsock *sk,
                              const struct tcp_fragment *f) {
        uint16_t peer_mss = sk->u.tcp.snd_mss;
        uint16_t option_bytes =
            f == NULL || f->opt_len < 0 ? 0 : (uint16_t)f->opt_len * 4;
        uint16_t local_mss = TCP_DEFAULT_MSS;

        if (peer_mss == 0)
                peer_mss = TCP_DEFAULT_MSS;
        if (option_bytes > local_mss)
                local_mss = 0;
        else
                local_mss -= option_bytes;
        return peer_mss < local_mss ? peer_mss : local_mss;
}
