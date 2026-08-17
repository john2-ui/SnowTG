/**
 * @file tcp_cc/internal.h
 * @brief Shared fixed-size helpers for built-in congestion controllers.
 */
#ifndef NETARCH_TCP_CC_INTERNAL_H
#define NETARCH_TCP_CC_INTERNAL_H

#include "../config.h"
#include "../tcp.h"

#include <limits.h>
#include <stdint.h>

/** Return the negotiated SMSS, falling back to the stack default. */
static inline uint32_t tcp_cc_smss(const struct tcp_stream *tp) {
        return tp->snd_mss != 0 ? tp->snd_mss : TCP_DEFAULT_MSS;
}

/** Add two congestion-window values without unsigned wraparound. */
static inline uint32_t tcp_cc_add_sat(uint32_t left, uint32_t right) {
        return UINT32_MAX - left < right ? UINT32_MAX : left + right;
}

/** Return the RFC 5681 initial congestion window for one SMSS. */
static inline uint32_t tcp_cc_initial_window(uint32_t smss) {
        if (smss > 2190U)
                return tcp_cc_add_sat(smss, smss);
        if (smss > 1095U)
                return tcp_cc_add_sat(tcp_cc_add_sat(smss, smss), smss);
        return tcp_cc_add_sat(tcp_cc_add_sat(smss, smss),
                              tcp_cc_add_sat(smss, smss));
}

/** Return max(flight * numerator / denominator, 2 * SMSS). */
static inline uint32_t tcp_cc_ssthresh(uint32_t flight, uint32_t smss,
                                      uint32_t numerator,
                                      uint32_t denominator) {
        uint32_t threshold =
            (uint32_t)(((uint64_t)flight * numerator) / denominator);
        uint32_t floor = tcp_cc_add_sat(smss, smss);

        return threshold > floor ? threshold : floor;
}

/**
 * @brief Apply RFC 6582 artificial inflation while non-SACK recovery runs.
 * @return True when recovery consumed the ACK and normal growth must stop.
 */
static inline bool
tcp_cc_newreno_recovery_ack(struct tcp_stream *tp,
                            const struct tcp_cc_ack_event *event) {
        uint32_t smss = tcp_cc_smss(tp);

        if (!event->in_recovery)
                return false;
        if (!event->newreno_recovery)
                return true;
        if (event->entered_recovery)
                return true;
        if (event->partial_ack) {
                tp->cc.cwnd = tp->cc.cwnd > event->acked_bytes
                                  ? tp->cc.cwnd - event->acked_bytes
                                  : 0;
                if (event->acked_bytes >= smss)
                        tp->cc.cwnd = tcp_cc_add_sat(tp->cc.cwnd, smss);
                if (tp->cc.cwnd < smss)
                        tp->cc.cwnd = smss;
                return true;
        }
        if (event->duplicate_ack)
                tp->cc.cwnd = tcp_cc_add_sat(tp->cc.cwnd, smss);
        return true;
}

#endif /* NETARCH_TCP_CC_INTERNAL_H */
