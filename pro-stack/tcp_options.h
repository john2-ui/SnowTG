#ifndef NETARCH_TCP_OPTIONS_H
#define NETARCH_TCP_OPTIONS_H

#include "tcp.h"

#include <stdbool.h>
#include <stdint.h>

struct nsock;

/**
 * @brief Decoded TCP options from one validated TCP header.
 *
 * MSS and Window Scale are consumed only while negotiating a SYN. Timestamp
 * is meaningful on every non-RST segment after it has been negotiated.
 */
struct tcp_options_rx {
        uint16_t mss;
        uint8_t wscale;
        bool wscale_present;
        bool timestamp_present;
        uint32_t tsval;
        uint32_t tsecr;
};

/** Reset per-connection TCP option negotiation state before an open. */
void tcp_options_reset_state(struct nsock *sk);

/** Return the local Timestamp clock in milliseconds, modulo 2^32. */
uint32_t tcp_options_now_ms(void);

/**
 * @brief Decode and validate all TCP options in @p hdr.
 * @return 0 on success, or -1 when the option area is malformed.
 */
int tcp_options_parse(const struct rte_tcp_hdr *hdr, struct tcp_options_rx *rx);

/**
 * @brief Apply the peer's SYN options after this endpoint offered WS and TS.
 */
void tcp_options_negotiate_syn(struct nsock *sk,
                               const struct tcp_options_rx *peer);

/**
 * @brief Encode MSS, optionally WS, and optionally Timestamp on a SYN.
 * @param tsecr Zero for an active SYN; peer SYN TSval for a SYN+ACK.
 * @return 0 on success, or -1 when the fragment option space is exhausted.
 */
int tcp_options_apply_syn(struct nsock *sk, struct tcp_fragment *f,
                          bool include_wscale, bool include_timestamp,
                          uint32_t tsecr);

/**
 * @brief Encode Timestamp on a post-SYN segment when it was negotiated.
 * @return 0 on success, or -1 when the fragment option space is exhausted.
 */
int tcp_options_apply_established(struct nsock *sk, struct tcp_fragment *f);

/**
 * @brief Validate and record Timestamp state on an inbound segment.
 *
 * The caller supplies sequence context after applying its receive-window
 * acceptance rule.  A nonzero @p seq_acceptable permits PAWS to reject a
 * stale Timestamp before the TCP state handler observes the segment.
 * @return 0 when acceptable, -1 when a negotiated Timestamp is absent, or
 *         -2 when PAWS rejects a stale Timestamp.
 */
int tcp_options_process_inbound(struct nsock *sk,
                                const struct tcp_options_rx *rx, bool is_rst,
                                uint32_t seg_seq, uint16_t seg_len,
                                bool has_fin, bool seq_acceptable);

/** Return the local payload limit after emitted post-SYN options. */
uint16_t tcp_options_data_mss(const struct nsock *sk);

#endif /* NETARCH_TCP_OPTIONS_H */
