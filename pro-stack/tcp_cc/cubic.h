/** @file tcp_cc/cubic.h @brief Built-in RFC 9438 CUBIC controller. */
#ifndef NETARCH_TCP_CC_CUBIC_H
#define NETARCH_TCP_CC_CUBIC_H

#include "../tcp_cc.h"

extern const struct tcp_cc_ops tcp_cubic_ops;

#ifdef TCP_TESTING
uint32_t tcp_cubic_test_k_ms(uint32_t w_max, uint32_t cwnd_epoch,
                             uint32_t smss);
uint32_t tcp_cubic_test_window_at(uint32_t w_max, uint32_t k_ms,
                                  uint32_t smss, uint32_t elapsed_ms);
uint32_t tcp_cubic_test_w_max(struct tcp_stream *tp);
#endif

#endif /* NETARCH_TCP_CC_CUBIC_H */
