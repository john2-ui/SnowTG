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
        uint16_t mss; /**< Peer MSS, or TCP_DEFAULT_MSS when absent/too small. */
        uint8_t wscale; /**< Validated peer Window Scale shift. */
        bool wscale_present; /**< A unique valid kind 3 was present. */
        bool sack_permitted_present; /**< SYN carried a valid kind 4. */
        uint8_t sack_count; /**< Valid kind 5 blocks on a non-SYN ACK. */
        struct tcp_sack_block sacks[TCP_SACK_MAX_BLOCKS]; /**< Host order. */
        bool timestamp_present; /**< A unique valid kind 8 was present. */
        uint32_t tsval; /**< Decoded Timestamp Value in host order. */
        uint32_t tsecr; /**< Decoded Timestamp Echo Reply in host order. */
};

/** Reset per-connection TCP option negotiation state before an open. */
void tcp_options_reset_state(struct nsock *sk);

/** Return the local Timestamp clock in milliseconds, modulo 2^32. */
uint32_t tcp_options_now_ms(void);

/**
 * @brief Decode and validate all TCP options in @p hdr.
 * @param hdr TCP header whose data offset bounds the option area.
 * @param rx Output reset and populated only with validated option values.
 * @return 0 on success, or -1 when the option area is malformed.
 */
int tcp_options_parse(const struct rte_tcp_hdr *hdr, struct tcp_options_rx *rx);

/**
 * @brief Apply peer SYN options and finalize local option negotiation.
 * @param sk Stream that sent or will send the corresponding local SYN.
 * @param peer Validated options decoded from the peer SYN.
 */
void tcp_options_negotiate_syn(struct nsock *sk,
                               const struct tcp_options_rx *peer);

/**
 * @brief Encode MSS, optionally WS, Timestamp, and SACK Permitted on a SYN.
 * @param sk Stream whose local capabilities and clocks are encoded.
 * @param f SYN or SYN+ACK fragment receiving the encoded options.
 * @param include_wscale Whether to append the local Window Scale offer.
 * @param include_timestamp Whether to append a Timestamp offer.
 * @param include_sack_permitted Whether to append RFC 2018 kind 4.
 * @param tsecr Zero for an active SYN; peer SYN TSval for a SYN+ACK.
 * @return 0 on success, or -1 when the fragment option space is exhausted.
 */
int tcp_options_apply_syn(struct nsock *sk, struct tcp_fragment *f,
                          bool include_wscale, bool include_timestamp,
                          bool include_sack_permitted, uint32_t tsecr);

/**
 * @brief Encode negotiated Timestamp and receiver SACK state post-SYN.
 * @param sk Stream supplying Timestamp, OFO history, and D-SACK state.
 * @param f Fragment whose flags and remaining option capacity are respected.
 * @return 0 on success, or -1 when the fragment option space is exhausted.
 */
int tcp_options_apply_established(struct nsock *sk, struct tcp_fragment *f);

/**
 * @brief Validate and record Timestamp state on an inbound segment.
 *
 * The caller supplies sequence context after applying its receive-window
 * acceptance rule.  A nonzero @p seq_acceptable permits PAWS to reject a
 * stale Timestamp before the TCP state handler observes the segment.
 * SACK blocks are copied into the per-packet TCB fields before this check so
 * ACK processing observes one coherent decoded packet.
 * @return 0 when acceptable, -1 when a negotiated Timestamp is absent, or
 *         -2 when PAWS rejects a stale Timestamp.
 */
int tcp_options_process_inbound(struct nsock *sk,
                                const struct tcp_options_rx *rx, bool is_rst,
                                uint32_t seg_seq, bool seq_acceptable);

/**
 * @brief Commit Timestamp state after in-order receive progress.
 *
 * Must be called only after the current segment's payload or FIN advanced
 * @c recv_ack.  This prevents a segment retained for later delivery from
 * changing the Timestamp echoed to the peer.
 */
void tcp_options_note_receive_progress(struct nsock *sk);

/** Return the local payload limit after the fragment's emitted options. */
uint16_t tcp_options_data_mss(const struct nsock *sk,
                              const struct tcp_fragment *f);

#endif /* NETARCH_TCP_OPTIONS_H */
