/**
 * @file tcp_cc.c
 * @brief Congestion-control dispatch layer and minimal RFC 5681 Reno.
 */
#include "tcp_cc.h"

#include "config.h"
#include "tcp.h"

#include <limits.h>
#include <string.h>

/** Return the RFC 5681 initial window derived from @p smss. */
static uint32_t tcp_reno_initial_window(uint32_t smss) {
        if (smss > 2190U)
                return 2U * smss;
        if (smss > 1095U)
                return 3U * smss;
        return 4U * smss;
}

/** Return max(FlightSize / 2, 2 * SMSS) without changing the TCB. */
static uint32_t tcp_reno_ssthresh(uint32_t flight, uint32_t smss) {
        uint32_t half = flight / 2U;
        uint32_t floor = 2U * smss;

        return half > floor ? half : floor;
}

/** Initialize Reno after the peer MSS and SYN retransmission state are known. */
static void tcp_reno_init(struct tcp_stream *tp, bool syn_retransmitted) {
        uint32_t smss = tp->snd_mss != 0 ? tp->snd_mss : TCP_DEFAULT_MSS;

        tp->cc.initial_window = tcp_reno_initial_window(smss);
        tp->cc.cwnd = syn_retransmitted ? smss : tp->cc.initial_window;
        tp->cc.ssthresh = UINT32_MAX;
        tp->cc.ca_acked = 0;
        tp->cc.rto_loss_valid = false;
        tp->cc.last_data_tx_ms = 0;
}

/** Reset Reno while retaining the vtable selected by the caller. */
static void tcp_reno_reset(struct tcp_stream *tp) {
        bool syn_retransmitted = tp->syn_retransmitted;

        memset(tp->cc.priv, 0, sizeof(tp->cc.priv));
        tcp_reno_init(tp, syn_retransmitted);
}

/**
 * @brief Grow Reno cwnd for newly cumulatively acknowledged payload.
 *
 * Slow start grows by at most one SMSS per ACK.  Congestion avoidance uses a
 * byte counter and adds one SMSS after approximately one cwnd was acknowledged.
 * Recovery ACKs are handled by the recovery state machine and do not grow cwnd.
 */
static void tcp_reno_on_ack(struct tcp_stream *tp,
                            const struct tcp_cc_ack_event *event) {
        uint32_t smss = tp->snd_mss != 0 ? tp->snd_mss : TCP_DEFAULT_MSS;
        uint32_t acked = event->acked_bytes;

        if (acked == 0 || event->in_recovery)
                return;
        tp->cc.rto_loss_valid = false;
        if (tp->cc.cwnd < tp->cc.ssthresh) {
                uint32_t increase = acked < smss ? acked : smss;

                if (UINT32_MAX - tp->cc.cwnd < increase)
                        tp->cc.cwnd = UINT32_MAX;
                else
                        tp->cc.cwnd += increase;
                return;
        }

        if (UINT32_MAX - tp->cc.ca_acked < acked)
                tp->cc.ca_acked = UINT32_MAX;
        else
                tp->cc.ca_acked += acked;
        while (tp->cc.ca_acked >= tp->cc.cwnd && tp->cc.cwnd != UINT32_MAX) {
                uint32_t old_cwnd = tp->cc.cwnd;

                tp->cc.ca_acked -= old_cwnd;
                if (UINT32_MAX - tp->cc.cwnd < smss) {
                        tp->cc.cwnd = UINT32_MAX;
                        break;
                }
                tp->cc.cwnd += smss;
        }
}

/** Reno currently needs no per-packet state beyond the common TX timestamp. */
static void tcp_reno_on_packet_sent(struct tcp_stream *tp, uint32_t bytes,
                                    bool retransmission) {
        (void)tp;
        (void)bytes;
        (void)retransmission;
}

/**
 * @brief Apply Reno's multiplicative decrease for a declared loss.
 *
 * SACK recovery starts at ssthresh; classic Reno inflates by three SMSS for the
 * three duplicate ACKs.  The RTO branch is shared with the dedicated callback
 * and avoids lowering ssthresh again for repeated timeout of the same SND.UNA.
 */
static void tcp_reno_on_loss(struct tcp_stream *tp,
                             const struct tcp_cc_loss_event *event) {
        uint32_t smss = tp->snd_mss != 0 ? tp->snd_mss : TCP_DEFAULT_MSS;
        uint32_t threshold = tcp_reno_ssthresh(event->flight_size, smss);

        if (event->reason == TCP_CC_LOSS_RTO) {
                if (event->first_rto_for_seq)
                        tp->cc.ssthresh = threshold;
                tp->cc.cwnd = smss;
                tp->cc.ca_acked = 0;
                return;
        }

        tp->cc.ssthresh = threshold;
        if (event->reason == TCP_CC_LOSS_DUPACK)
                tp->cc.cwnd = threshold + 3U * smss;
        else
                tp->cc.cwnd = threshold;
        tp->cc.ca_acked = 0;
}

/** Deflate classic fast recovery to ssthresh and reset additive accounting. */
static void tcp_reno_on_recovery_exit(struct tcp_stream *tp) {
        if (tp->cc.cwnd > tp->cc.ssthresh)
                tp->cc.cwnd = tp->cc.ssthresh;
        tp->cc.ca_acked = 0;
}

/** Dedicated RTO entry point; Reno uses the common loss arithmetic above. */
static void tcp_reno_on_rto(struct tcp_stream *tp,
                            const struct tcp_cc_loss_event *event) {
        tcp_reno_on_loss(tp, event);
}

/** Limit restart-after-idle to the current RFC 5681 initial window. */
static void tcp_reno_on_idle_restart(struct tcp_stream *tp) {
        if (tp->cc.cwnd > tp->cc.initial_window)
                tp->cc.cwnd = tp->cc.initial_window;
}

/**
 * @brief Expose D-SACK to the vtable without non-standard Reno undo.
 *
 * Future algorithms may use this event for spurious-retransmission recovery;
 * the default Reno deliberately leaves cwnd and ssthresh unchanged.
 */
static void tcp_reno_on_dsack(struct tcp_stream *tp,
                              const struct tcp_sack_block *block,
                              bool covers_retransmission) {
        (void)tp;
        (void)block;
        (void)covers_retransmission;
}

/** Built-in RFC 5681 Reno implementation. */
const struct tcp_cc_ops tcp_reno_ops = {
    .name = "reno",
    .priv_size = 0,
    .init = tcp_reno_init,
    .reset = tcp_reno_reset,
    .on_ack = tcp_reno_on_ack,
    .on_packet_sent = tcp_reno_on_packet_sent,
    .on_loss = tcp_reno_on_loss,
    .on_recovery_exit = tcp_reno_on_recovery_exit,
    .on_rto = tcp_reno_on_rto,
    .on_idle_restart = tcp_reno_on_idle_restart,
    .on_dsack = tcp_reno_on_dsack,
};

/** @copydoc tcp_cc_set_ops */
void tcp_cc_set_ops(struct tcp_stream *tp, const struct tcp_cc_ops *ops,
                    bool syn_retransmitted) {
        if (tp == NULL || ops == NULL || ops->priv_size > TCP_CC_PRIV_SIZE)
                return;
        memset(&tp->cc, 0, sizeof(tp->cc));
        tp->cc.ops = ops;
        if (ops->init != NULL)
                ops->init(tp, syn_retransmitted);
}

/** @copydoc tcp_cc_use_reno */
void tcp_cc_use_reno(struct tcp_stream *tp, bool syn_retransmitted) {
        tcp_cc_set_ops(tp, &tcp_reno_ops, syn_retransmitted);
}

/** @copydoc tcp_cc_reset */
void tcp_cc_reset(struct tcp_stream *tp) {
        if (tp != NULL && tp->cc.ops != NULL && tp->cc.ops->reset != NULL)
                tp->cc.ops->reset(tp);
}

/** @copydoc tcp_cc_on_ack */
void tcp_cc_on_ack(struct tcp_stream *tp,
                   const struct tcp_cc_ack_event *event) {
        if (tp->cc.ops != NULL && tp->cc.ops->on_ack != NULL)
                tp->cc.ops->on_ack(tp, event);
}

/** @copydoc tcp_cc_on_packet_sent */
void tcp_cc_on_packet_sent(struct tcp_stream *tp, uint32_t bytes,
                           bool retransmission, uint32_t now_ms) {
        tp->cc.last_data_tx_ms = now_ms;
        if (tp->cc.ops != NULL && tp->cc.ops->on_packet_sent != NULL)
                tp->cc.ops->on_packet_sent(tp, bytes, retransmission);
}

/** @copydoc tcp_cc_on_loss */
void tcp_cc_on_loss(struct tcp_stream *tp,
                    const struct tcp_cc_loss_event *event) {
        if (tp->cc.ops != NULL && tp->cc.ops->on_loss != NULL)
                tp->cc.ops->on_loss(tp, event);
}

/** @copydoc tcp_cc_on_recovery_exit */
void tcp_cc_on_recovery_exit(struct tcp_stream *tp) {
        if (tp->cc.ops != NULL && tp->cc.ops->on_recovery_exit != NULL)
                tp->cc.ops->on_recovery_exit(tp);
}

/** @copydoc tcp_cc_on_rto */
void tcp_cc_on_rto(struct tcp_stream *tp,
                   const struct tcp_cc_loss_event *event) {
        if (tp->cc.ops != NULL && tp->cc.ops->on_rto != NULL)
                tp->cc.ops->on_rto(tp, event);
}

/** @copydoc tcp_cc_on_idle_restart */
void tcp_cc_on_idle_restart(struct tcp_stream *tp, uint32_t now_ms,
                            uint32_t rto_ms) {
        if (tp->cc.last_data_tx_ms != 0 &&
            (uint32_t)(now_ms - tp->cc.last_data_tx_ms) > rto_ms &&
            tp->cc.ops != NULL && tp->cc.ops->on_idle_restart != NULL)
                tp->cc.ops->on_idle_restart(tp);
}

/** @copydoc tcp_cc_on_dsack */
void tcp_cc_on_dsack(struct tcp_stream *tp,
                     const struct tcp_sack_block *block,
                     bool covers_retransmission) {
        if (tp->cc.ops != NULL && tp->cc.ops->on_dsack != NULL)
                tp->cc.ops->on_dsack(tp, block, covers_retransmission);
}

/** @copydoc tcp_cc_send_window */
uint32_t tcp_cc_send_window(const struct tcp_stream *tp) {
        return tp->cc.ops == NULL ? UINT32_MAX : tp->cc.cwnd;
}
