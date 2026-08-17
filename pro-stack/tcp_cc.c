/**
 * @file tcp_cc.c
 * @brief Algorithm-independent TCP congestion-control dispatch layer.
 */
#include "tcp_cc.h"

#include "config.h"
#include "tcp.h"
#include "tcp_cc/cubic.h"
#include "tcp_cc/newreno.h"

#include <limits.h>
#include <string.h>

/** Return the callback table selected for new connections in config.h. */
static const struct tcp_cc_ops *tcp_cc_default_ops(void) {
#if TCP_CC_DEFAULT_ALGO == TCP_CC_ALGO_CUBIC
        return &tcp_cubic_ops;
#else
        return &tcp_newreno_ops;
#endif
}

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

/** @copydoc tcp_cc_init_default */
void tcp_cc_init_default(struct tcp_stream *tp, bool syn_retransmitted) {
        tcp_cc_set_ops(tp, tcp_cc_default_ops(), syn_retransmitted);
}

/** @copydoc tcp_cc_reset */
void tcp_cc_reset(struct tcp_stream *tp) {
        if (tp != NULL && tp->cc.ops != NULL && tp->cc.ops->reset != NULL)
                tp->cc.ops->reset(tp);
}

/** @copydoc tcp_cc_on_ack */
void tcp_cc_on_ack(struct tcp_stream *tp,
                   const struct tcp_cc_ack_event *event) {
        if (tp != NULL && event != NULL && tp->cc.ops != NULL &&
            tp->cc.ops->on_ack != NULL)
                tp->cc.ops->on_ack(tp, event);
}

/** @copydoc tcp_cc_on_packet_sent */
void tcp_cc_on_packet_sent(struct tcp_stream *tp,
                           const struct tcp_cc_tx_event *event) {
        if (tp == NULL || event == NULL)
                return;
        tp->cc.last_data_tx_ms = event->now_ms;
        tp->cc.cwnd_limited = event->cwnd_limited;
        if (tp->cc.ops != NULL && tp->cc.ops->on_packet_sent != NULL)
                tp->cc.ops->on_packet_sent(tp, event);
}

/** @copydoc tcp_cc_on_loss */
void tcp_cc_on_loss(struct tcp_stream *tp,
                    const struct tcp_cc_loss_event *event) {
        if (tp != NULL && event != NULL && tp->cc.ops != NULL &&
            tp->cc.ops->on_loss != NULL)
                tp->cc.ops->on_loss(tp, event);
}

/** @copydoc tcp_cc_on_recovery_exit */
void tcp_cc_on_recovery_exit(struct tcp_stream *tp) {
        if (tp != NULL && tp->cc.ops != NULL &&
            tp->cc.ops->on_recovery_exit != NULL)
                tp->cc.ops->on_recovery_exit(tp);
}

/** @copydoc tcp_cc_on_rto */
void tcp_cc_on_rto(struct tcp_stream *tp,
                   const struct tcp_cc_loss_event *event) {
        if (tp != NULL && event != NULL && tp->cc.ops != NULL &&
            tp->cc.ops->on_rto != NULL)
                tp->cc.ops->on_rto(tp, event);
}

/** @copydoc tcp_cc_on_idle_restart */
void tcp_cc_on_idle_restart(struct tcp_stream *tp, uint32_t now_ms,
                            uint32_t rto_ms) {
        if (tp != NULL && tp->cc.last_data_tx_ms != 0 &&
            (uint32_t)(now_ms - tp->cc.last_data_tx_ms) > rto_ms &&
            tp->cc.ops != NULL && tp->cc.ops->on_idle_restart != NULL)
                tp->cc.ops->on_idle_restart(tp);
}

/** @copydoc tcp_cc_on_dsack */
void tcp_cc_on_dsack(struct tcp_stream *tp,
                     const struct tcp_sack_block *block,
                     bool covers_retransmission) {
        if (tp != NULL && tp->cc.ops != NULL && tp->cc.ops->on_dsack != NULL)
                tp->cc.ops->on_dsack(tp, block, covers_retransmission);
}

/** @copydoc tcp_cc_send_window */
uint32_t tcp_cc_send_window(const struct tcp_stream *tp) {
        return tp == NULL || tp->cc.ops == NULL ? UINT32_MAX : tp->cc.cwnd;
}
