/**
 * @file tcp_cc/cubic.c
 * @brief Integer RFC 9438 CUBIC with first-slow-start HyStart++.
 */
#include "cubic.h"

#include "internal.h"

#include <limits.h>
#include <string.h>

/* __extension__ keeps strict test builds quiet while retaining wide, exact
 * intermediates for the cubic polynomial.  No floating point enters TX/RX. */
__extension__ typedef unsigned __int128 tcp_cc_u128;

#define CUBIC_BETA_NUM 7U
#define CUBIC_BETA_DEN 10U
#define CUBIC_C_NUM 2U
#define CUBIC_C_DEN 5U
#define CUBIC_TIME_SCALE 1000000000ULL /* 1000 ms cubed */

#define HYSTART_MIN_SAMPLES 8U
#define HYSTART_MIN_THRESH_MS 4U
#define HYSTART_MAX_THRESH_MS 16U
#define HYSTART_CSS_ROUNDS 5U
#define HYSTART_SS_GROWTH 8U
#define HYSTART_CSS_DIVISOR 4U

enum cubic_phase {
        CUBIC_PHASE_SLOW_START = 0,
        CUBIC_PHASE_CSS,
        CUBIC_PHASE_AVOIDANCE,
};

enum cubic_flags {
        CUBIC_F_EPOCH_VALID = 1U << 0,
        CUBIC_F_LAST_UPDATE_VALID = 1U << 1,
        CUBIC_F_ROUND_VALID = 1U << 2,
        CUBIC_F_HYSTART_DONE = 1U << 3,
        CUBIC_F_AFTER_TIMEOUT = 1U << 4,
};

/** Fixed per-TCB CUBIC and HyStart++ state kept inside tcp_cc_state.priv. */
struct cubic_state {
        uint64_t cubic_credit; /**< Fractional byte-growth numerator. */
        uint32_t w_max; /**< Fast-convergence plateau in bytes. */
        uint32_t w_last_max; /**< Plateau preceding the latest event. */
        uint32_t cwnd_prior; /**< cwnd when ssthresh was last established. */
        uint32_t cwnd_epoch; /**< cwnd at the start of this CA epoch. */
        uint32_t w_est; /**< Reno-friendly window estimate in bytes. */
        uint32_t k_ms; /**< Time from epoch start to W_max. */
        uint32_t epoch_start_ms; /**< Application-time CA epoch origin. */
        uint32_t last_update_ms; /**< Last ACK used to pause epoch time. */
        uint32_t round_end; /**< HyStart++ SND.NXT round boundary. */
        uint32_t last_round_min_rtt; /**< Minimum RTT in previous round. */
        uint32_t current_round_min_rtt; /**< Minimum RTT in current round. */
        uint32_t css_baseline_min_rtt; /**< RTT baseline on CSS entry. */
        uint8_t sample_count; /**< Valid RTT samples in current round. */
        uint8_t css_rounds; /**< Completed Conservative Slow Start rounds. */
        uint8_t phase; /**< enum cubic_phase. */
        uint8_t flags; /**< enum cubic_flags bitmap. */
};

_Static_assert(sizeof(struct cubic_state) <= TCP_CC_PRIV_SIZE,
               "CUBIC private state exceeds tcp_cc_state.priv");
_Static_assert(_Alignof(struct cubic_state) <= _Alignof(uint64_t),
               "CUBIC private state requires stronger alignment");

static struct cubic_state *cubic_state(struct tcp_stream *tp) {
        return (struct cubic_state *)(void *)tp->cc.priv;
}

/** Return floor(cuberoot(value)) using overflow-free wide products. */
static uint32_t cubic_root(tcp_cc_u128 value) {
        uint64_t low = 0;
        uint64_t high = UINT32_MAX;

        while (low < high) {
                uint64_t mid = low + (high - low + 1U) / 2U;
                tcp_cc_u128 cube = (tcp_cc_u128)mid * mid * mid;

                if (cube <= value)
                        low = mid;
                else
                        high = mid - 1U;
        }
        return (uint32_t)low;
}

/** Calculate K in milliseconds from W_max and the epoch-start cwnd. */
static uint32_t cubic_k_ms(uint32_t w_max, uint32_t cwnd_epoch,
                           uint32_t smss) {
        if (w_max <= cwnd_epoch || smss == 0)
                return 0;

        tcp_cc_u128 scaled = (tcp_cc_u128)(w_max - cwnd_epoch) *
                             CUBIC_C_DEN * CUBIC_TIME_SCALE;
        scaled /= (tcp_cc_u128)CUBIC_C_NUM * smss;
        return cubic_root(scaled);
}

/** Evaluate C*(t-K)^3+W_max in byte units with saturation. */
static uint32_t cubic_window_at(const struct cubic_state *ca, uint32_t smss,
                                uint32_t elapsed_ms) {
        int64_t offset = (int64_t)elapsed_ms - ca->k_ms;
        uint64_t distance = offset < 0 ? (uint64_t)(-offset)
                                       : (uint64_t)offset;
        tcp_cc_u128 delta = (tcp_cc_u128)distance * distance * distance;

        delta *= (tcp_cc_u128)smss * CUBIC_C_NUM;
        delta /= (tcp_cc_u128)CUBIC_C_DEN * CUBIC_TIME_SCALE;
        if (offset < 0) {
                if (delta >= ca->w_max)
                        return 0;
                return ca->w_max - (uint32_t)delta;
        }
        if (delta > UINT32_MAX - ca->w_max)
                return UINT32_MAX;
        return ca->w_max + (uint32_t)delta;
}

/** Reset only the current CUBIC avoidance epoch. */
static void cubic_reset_epoch(struct cubic_state *ca) {
        ca->cubic_credit = 0;
        ca->cwnd_epoch = 0;
        ca->w_est = 0;
        ca->k_ms = 0;
        ca->epoch_start_ms = 0;
        ca->last_update_ms = 0;
        ca->flags &= (uint8_t)~(CUBIC_F_EPOCH_VALID |
                                CUBIC_F_LAST_UPDATE_VALID);
}

/** Initialize both CUBIC and the first-connection HyStart++ detector. */
static void cubic_init(struct tcp_stream *tp, bool syn_retransmitted) {
        struct cubic_state *ca = cubic_state(tp);
        uint32_t smss = tcp_cc_smss(tp);

        memset(ca, 0, sizeof(*ca));
        tp->cc.initial_window = tcp_cc_initial_window(smss);
        tp->cc.cwnd = syn_retransmitted ? smss : tp->cc.initial_window;
        tp->cc.ssthresh = UINT32_MAX;
        tp->cc.ca_acked = 0;
        tp->cc.rto_loss_valid = false;
        tp->cc.last_data_tx_ms = 0;
        tp->cc.cwnd_limited = false;
        ca->phase = CUBIC_PHASE_SLOW_START;
}

/** Reset a reused connection without changing its selected callback table. */
static void cubic_reset(struct tcp_stream *tp) {
        cubic_init(tp, tp->syn_retransmitted);
}

/** Begin a new HyStart++ round at the current SND.NXT boundary. */
static void hystart_begin_round(struct cubic_state *ca, uint32_t snd_nxt) {
        ca->round_end = snd_nxt;
        ca->current_round_min_rtt = UINT32_MAX;
        ca->sample_count = 0;
        ca->flags |= CUBIC_F_ROUND_VALID;
}

/** Complete one HyStart++ round and advance or leave CSS as required. */
static void hystart_finish_round(struct tcp_stream *tp,
                                 struct cubic_state *ca) {
        if (ca->sample_count != 0) {
                if (ca->phase == CUBIC_PHASE_CSS) {
                        if (ca->current_round_min_rtt <
                            ca->css_baseline_min_rtt) {
                                ca->phase = CUBIC_PHASE_SLOW_START;
                                ca->css_rounds = 0;
                        } else if (++ca->css_rounds >= HYSTART_CSS_ROUNDS) {
                                ca->phase = CUBIC_PHASE_AVOIDANCE;
                                ca->flags |= CUBIC_F_HYSTART_DONE;
                                tp->cc.ssthresh = tp->cc.cwnd;
                                ca->cwnd_prior = tp->cc.cwnd;
                                cubic_reset_epoch(ca);
                        }
                }
                ca->last_round_min_rtt = ca->current_round_min_rtt;
        }
}

/** Feed one safe Timestamp RTT sample to the HyStart++ delay detector. */
static void hystart_update(struct tcp_stream *tp, struct cubic_state *ca,
                           const struct tcp_cc_ack_event *event) {
        if ((ca->flags & CUBIC_F_HYSTART_DONE) != 0 ||
            ca->phase == CUBIC_PHASE_AVOIDANCE || !event->rtt_sample_valid)
                return;

        if ((ca->flags & CUBIC_F_ROUND_VALID) == 0)
                hystart_begin_round(ca, event->snd_nxt);
        else if ((int32_t)(event->ack_seq - ca->round_end) >= 0) {
                hystart_finish_round(tp, ca);
                if (ca->phase == CUBIC_PHASE_AVOIDANCE)
                        return;
                hystart_begin_round(ca, event->snd_nxt);
        }

        if (event->rtt_sample_ms < ca->current_round_min_rtt)
                ca->current_round_min_rtt = event->rtt_sample_ms;
        if (ca->sample_count != UINT8_MAX)
                ca->sample_count++;

        if (ca->phase == CUBIC_PHASE_SLOW_START &&
            ca->last_round_min_rtt != 0 &&
            ca->sample_count >= HYSTART_MIN_SAMPLES) {
                uint32_t threshold = ca->last_round_min_rtt / 8U;

                if (threshold < HYSTART_MIN_THRESH_MS)
                        threshold = HYSTART_MIN_THRESH_MS;
                if (threshold > HYSTART_MAX_THRESH_MS)
                        threshold = HYSTART_MAX_THRESH_MS;
                if (ca->current_round_min_rtt >=
                    ca->last_round_min_rtt + threshold) {
                        ca->phase = CUBIC_PHASE_CSS;
                        ca->css_rounds = 0;
                        ca->css_baseline_min_rtt =
                            ca->current_round_min_rtt;
                }
        }
}

/** Initialize a CUBIC congestion-avoidance epoch on its first eligible ACK. */
static void cubic_begin_epoch(struct tcp_stream *tp, struct cubic_state *ca,
                              uint32_t now_ms) {
        uint32_t smss = tcp_cc_smss(tp);

        ca->cwnd_epoch = tp->cc.cwnd;
        ca->w_est = tp->cc.cwnd;
        if ((ca->flags & CUBIC_F_AFTER_TIMEOUT) != 0 || ca->w_max == 0) {
                ca->w_max = tp->cc.cwnd;
                ca->k_ms = 0;
                ca->flags &= (uint8_t)~CUBIC_F_AFTER_TIMEOUT;
        } else {
                ca->k_ms = cubic_k_ms(ca->w_max, ca->cwnd_epoch, smss);
        }
        ca->epoch_start_ms = now_ms;
        ca->last_update_ms = now_ms;
        ca->cubic_credit = 0;
        ca->flags |= CUBIC_F_EPOCH_VALID | CUBIC_F_LAST_UPDATE_VALID;
}

/** Advance RFC 9438 W_est and the bounded cubic target for one new ACK. */
static void cubic_avoidance_ack(struct tcp_stream *tp, struct cubic_state *ca,
                                const struct tcp_cc_ack_event *event) {
        uint32_t smss = tcp_cc_smss(tp);

        if ((ca->flags & CUBIC_F_EPOCH_VALID) == 0)
                cubic_begin_epoch(tp, ca, event->now_ms);

        if (!event->cwnd_limited) {
                if ((ca->flags & CUBIC_F_LAST_UPDATE_VALID) != 0)
                        ca->epoch_start_ms +=
                            event->now_ms - ca->last_update_ms;
                ca->last_update_ms = event->now_ms;
                ca->flags |= CUBIC_F_LAST_UPDATE_VALID;
                return;
        }
        ca->last_update_ms = event->now_ms;
        ca->flags |= CUBIC_F_LAST_UPDATE_VALID;

        uint32_t alpha_num = ca->w_est >= ca->cwnd_prior ? 1U : 9U;
        uint32_t alpha_den = ca->w_est >= ca->cwnd_prior ? 1U : 17U;
        uint64_t friendly_inc = (uint64_t)alpha_num * event->acked_bytes * smss;
        friendly_inc /= (uint64_t)alpha_den *
                        (tp->cc.cwnd != 0 ? tp->cc.cwnd : smss);
        ca->w_est = tcp_cc_add_sat(ca->w_est, (uint32_t)friendly_inc);

        uint32_t elapsed = event->now_ms - ca->epoch_start_ms;
        uint32_t rtt = tp->srtt_ms != 0 ? tp->srtt_ms
                                       : (event->rtt_sample_valid
                                              ? event->rtt_sample_ms
                                              : 1U);
        uint32_t target = cubic_window_at(ca, smss, elapsed + rtt);
        uint32_t upper = tcp_cc_add_sat(tp->cc.cwnd, tp->cc.cwnd / 2U);

        if (target < tp->cc.cwnd)
                target = tp->cc.cwnd;
        if (target > upper)
                target = upper;
        if (target > tp->cc.cwnd && tp->cc.cwnd != UINT32_MAX) {
                ca->cubic_credit +=
                    (uint64_t)(target - tp->cc.cwnd) * smss;
                uint32_t increase = (uint32_t)(ca->cubic_credit /
                                               tp->cc.cwnd);
                ca->cubic_credit %= tp->cc.cwnd;
                tp->cc.cwnd = tcp_cc_add_sat(tp->cc.cwnd, increase);
        }
        if (ca->w_est > tp->cc.cwnd)
                tp->cc.cwnd = ca->w_est;
}

/** Apply HyStart++ slow start or CUBIC congestion avoidance to a new ACK. */
static void cubic_on_ack(struct tcp_stream *tp,
                         const struct tcp_cc_ack_event *event) {
        struct cubic_state *ca = cubic_state(tp);
        uint32_t smss = tcp_cc_smss(tp);

        if (tcp_cc_newreno_recovery_ack(tp, event) ||
            event->acked_bytes == 0)
                return;
        tp->cc.rto_loss_valid = false;

        if (tp->cc.cwnd < tp->cc.ssthresh &&
            ca->phase != CUBIC_PHASE_AVOIDANCE) {
                hystart_update(tp, ca, event);
                if (!event->cwnd_limited)
                        return;
                uint32_t cap =
                    (uint32_t)((uint64_t)smss * HYSTART_SS_GROWTH > UINT32_MAX
                                   ? UINT32_MAX
                                   : (uint64_t)smss * HYSTART_SS_GROWTH);
                uint32_t increase = event->acked_bytes < cap
                                        ? event->acked_bytes
                                        : cap;

                if (ca->phase == CUBIC_PHASE_CSS)
                        increase /= HYSTART_CSS_DIVISOR;
                if (increase == 0)
                        increase = 1;
                tp->cc.cwnd = tcp_cc_add_sat(tp->cc.cwnd, increase);
                return;
        }

        if (ca->phase != CUBIC_PHASE_AVOIDANCE) {
                ca->phase = CUBIC_PHASE_AVOIDANCE;
                ca->flags |= CUBIC_F_HYSTART_DONE;
                ca->cwnd_prior = tp->cc.cwnd;
                cubic_reset_epoch(ca);
        }
        cubic_avoidance_ack(tp, ca, event);
}

/** CUBIC gets ACK-clock and application-limited state from ACK/TX events. */
static void cubic_on_packet_sent(struct tcp_stream *tp,
                                 const struct tcp_cc_tx_event *event) {
        (void)tp;
        (void)event;
}

/** Apply beta=0.7, fast convergence, and RFC 6582 entry inflation. */
static void cubic_on_loss(struct tcp_stream *tp,
                          const struct tcp_cc_loss_event *event) {
        struct cubic_state *ca = cubic_state(tp);
        uint32_t smss = tcp_cc_smss(tp);
        uint32_t threshold = tcp_cc_ssthresh(
            event->flight_size, smss, CUBIC_BETA_NUM, CUBIC_BETA_DEN);

        if (event->reason == TCP_CC_LOSS_RTO) {
                if (event->first_rto_for_seq) {
                        ca->cwnd_prior = tp->cc.cwnd;
                        tp->cc.ssthresh = threshold;
                }
                tp->cc.cwnd = smss;
                ca->phase = CUBIC_PHASE_SLOW_START;
                ca->flags |= CUBIC_F_HYSTART_DONE | CUBIC_F_AFTER_TIMEOUT;
                ca->flags &= (uint8_t)~CUBIC_F_ROUND_VALID;
                cubic_reset_epoch(ca);
                return;
        }

        ca->cwnd_prior = tp->cc.cwnd;
        ca->w_last_max = ca->w_max;
        if (ca->w_max != 0 && tp->cc.cwnd < ca->w_max)
                ca->w_max = (uint32_t)(((uint64_t)tp->cc.cwnd *
                                        (CUBIC_BETA_DEN + CUBIC_BETA_NUM)) /
                                       (2U * CUBIC_BETA_DEN));
        else
                ca->w_max = tp->cc.cwnd;

        tp->cc.ssthresh = threshold;
        tp->cc.cwnd = event->reason == TCP_CC_LOSS_DUPACK
                          ? tcp_cc_add_sat(
                                threshold,
                                tcp_cc_add_sat(
                                    smss, tcp_cc_add_sat(smss, smss)))
                          : threshold;
        tp->cc.ca_acked = 0;
        ca->phase = CUBIC_PHASE_AVOIDANCE;
        ca->flags |= CUBIC_F_HYSTART_DONE;
        cubic_reset_epoch(ca);
}

/** Remove RFC 6582 artificial inflation without changing the CUBIC plateau. */
static void cubic_on_recovery_exit(struct tcp_stream *tp) {
        struct cubic_state *ca = cubic_state(tp);

        if (tp->cc.cwnd > tp->cc.ssthresh)
                tp->cc.cwnd = tp->cc.ssthresh;
        tp->cc.ca_acked = 0;
        cubic_reset_epoch(ca);
}

/** RTO follows Reno cwnd collapse but retains CUBIC's beta for ssthresh. */
static void cubic_on_rto(struct tcp_stream *tp,
                         const struct tcp_cc_loss_event *event) {
        cubic_on_loss(tp, event);
}

/** Restart an idle flow at IW and begin a fresh cubic epoch later. */
static void cubic_on_idle_restart(struct tcp_stream *tp) {
        struct cubic_state *ca = cubic_state(tp);

        if (tp->cc.cwnd > tp->cc.initial_window)
                tp->cc.cwnd = tp->cc.initial_window;
        cubic_reset_epoch(ca);
}

/** RFC 9438 spurious-loss undo remains intentionally disabled. */
static void cubic_on_dsack(struct tcp_stream *tp,
                           const struct tcp_sack_block *block,
                           bool covers_retransmission) {
        (void)tp;
        (void)block;
        (void)covers_retransmission;
}

/** Built-in RFC 9438 CUBIC callback table. */
const struct tcp_cc_ops tcp_cubic_ops = {
    .name = "cubic",
    .priv_size = sizeof(struct cubic_state),
    .init = cubic_init,
    .reset = cubic_reset,
    .on_ack = cubic_on_ack,
    .on_packet_sent = cubic_on_packet_sent,
    .on_loss = cubic_on_loss,
    .on_recovery_exit = cubic_on_recovery_exit,
    .on_rto = cubic_on_rto,
    .on_idle_restart = cubic_on_idle_restart,
    .on_dsack = cubic_on_dsack,
};

#ifdef TCP_TESTING
/** Test seam for deterministic fixed-point K vectors. */
uint32_t tcp_cubic_test_k_ms(uint32_t w_max, uint32_t cwnd_epoch,
                             uint32_t smss) {
        return cubic_k_ms(w_max, cwnd_epoch, smss);
}

/** Test seam for deterministic concave/plateau/convex window vectors. */
uint32_t tcp_cubic_test_window_at(uint32_t w_max, uint32_t k_ms,
                                  uint32_t smss, uint32_t elapsed_ms) {
        struct cubic_state ca;

        memset(&ca, 0, sizeof(ca));
        ca.w_max = w_max;
        ca.k_ms = k_ms;
        return cubic_window_at(&ca, smss, elapsed_ms);
}

/** Test seam exposing only the current RFC 9438 fast-convergence plateau. */
uint32_t tcp_cubic_test_w_max(struct tcp_stream *tp) {
        return cubic_state(tp)->w_max;
}
#endif
