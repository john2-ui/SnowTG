/**
 * @file tcp_cc/newreno.c
 * @brief BSD-style NewReno congestion control with RFC 6582 recovery.
 */
#include "newreno.h"

#include "internal.h"

#include <limits.h>
#include <string.h>

/** Initialize NewReno after MSS negotiation and SYN retransmission handling. */
static void newreno_init(struct tcp_stream *tp, bool syn_retransmitted) {
        uint32_t smss = tcp_cc_smss(tp);

        tp->cc.initial_window = tcp_cc_initial_window(smss);
        tp->cc.cwnd = syn_retransmitted ? smss : tp->cc.initial_window;
        tp->cc.ssthresh = UINT32_MAX;
        tp->cc.ca_acked = 0;
        tp->cc.rto_loss_valid = false;
        tp->cc.last_data_tx_ms = 0;
        tp->cc.cwnd_limited = false;
}

/** Reset per-flight state while retaining the selected NewReno vtable. */
static void newreno_reset(struct tcp_stream *tp) {
        memset(tp->cc.priv, 0, sizeof(tp->cc.priv));
        newreno_init(tp, tp->syn_retransmitted);
}

/** Grow cwnd using RFC 5681 slow start and byte-counted avoidance. */
static void newreno_on_ack(struct tcp_stream *tp,
                           const struct tcp_cc_ack_event *event) {
        uint32_t smss = tcp_cc_smss(tp);
        uint32_t acked = event->acked_bytes;

        if (tcp_cc_newreno_recovery_ack(tp, event) || acked == 0)
                return;
        tp->cc.rto_loss_valid = false;
        if (tp->cc.cwnd < tp->cc.ssthresh) {
                uint32_t increase = acked < smss ? acked : smss;

                tp->cc.cwnd = tcp_cc_add_sat(tp->cc.cwnd, increase);
                return;
        }

        tp->cc.ca_acked = tcp_cc_add_sat(tp->cc.ca_acked, acked);
        while (tp->cc.cwnd != UINT32_MAX &&
               tp->cc.ca_acked >= tp->cc.cwnd) {
                uint32_t old_cwnd = tp->cc.cwnd;

                tp->cc.ca_acked -= old_cwnd;
                tp->cc.cwnd = tcp_cc_add_sat(tp->cc.cwnd, smss);
        }
}

/** NewReno keeps no per-packet private state. */
static void newreno_on_packet_sent(struct tcp_stream *tp,
                                   const struct tcp_cc_tx_event *event) {
        (void)tp;
        (void)event;
}

/** Apply the NewReno 1/2 multiplicative decrease. */
static void newreno_on_loss(struct tcp_stream *tp,
                            const struct tcp_cc_loss_event *event) {
        uint32_t smss = tcp_cc_smss(tp);
        uint32_t threshold = tcp_cc_ssthresh(event->flight_size, smss, 1, 2);

        if (event->reason == TCP_CC_LOSS_RTO) {
                if (event->first_rto_for_seq)
                        tp->cc.ssthresh = threshold;
                tp->cc.cwnd = smss;
        } else {
                tp->cc.ssthresh = threshold;
                tp->cc.cwnd = event->reason == TCP_CC_LOSS_DUPACK
                                  ? tcp_cc_add_sat(
                                        threshold,
                                        tcp_cc_add_sat(
                                            smss, tcp_cc_add_sat(smss, smss)))
                                  : threshold;
        }
        tp->cc.ca_acked = 0;
}

/** Deflate the artificial RFC 6582 recovery window. */
static void newreno_on_recovery_exit(struct tcp_stream *tp) {
        if (tp->cc.cwnd > tp->cc.ssthresh)
                tp->cc.cwnd = tp->cc.ssthresh;
        tp->cc.ca_acked = 0;
}

/** RTO uses the same loss response with repeated-timeout protection. */
static void newreno_on_rto(struct tcp_stream *tp,
                           const struct tcp_cc_loss_event *event) {
        newreno_on_loss(tp, event);
}

/** RFC 5681 restart-after-idle limits cwnd to the initial window. */
static void newreno_on_idle_restart(struct tcp_stream *tp) {
        if (tp->cc.cwnd > tp->cc.initial_window)
                tp->cc.cwnd = tp->cc.initial_window;
        tp->cc.ca_acked = 0;
}

/** D-SACK is observable but does not undo a congestion response. */
static void newreno_on_dsack(struct tcp_stream *tp,
                             const struct tcp_sack_block *block,
                             bool covers_retransmission) {
        (void)tp;
        (void)block;
        (void)covers_retransmission;
}

/** Built-in BSD-style NewReno callback table. */
const struct tcp_cc_ops tcp_newreno_ops = {
    .name = "newreno",
    .priv_size = 0,
    .init = newreno_init,
    .reset = newreno_reset,
    .on_ack = newreno_on_ack,
    .on_packet_sent = newreno_on_packet_sent,
    .on_loss = newreno_on_loss,
    .on_recovery_exit = newreno_on_recovery_exit,
    .on_rto = newreno_on_rto,
    .on_idle_restart = newreno_on_idle_restart,
    .on_dsack = newreno_on_dsack,
};
