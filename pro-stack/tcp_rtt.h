#ifndef NETARCH_TCP_RTT_H
#define NETARCH_TCP_RTT_H

#include <stdbool.h>
#include <stdint.h>

struct nsock;

/** Reset one stream's RFC 6298 data/FIN retransmission state. */
void tcp_rtt_reset(struct nsock *sk);
/** Record the first new Timestamp-bearing data segment in a flight. */
void tcp_rtt_note_xmit(struct nsock *sk, uint32_t end_seq, uint32_t tsval);
/** Update the estimator from a matching Timestamp echo on an advancing ACK. */
bool tcp_rtt_on_ack(struct nsock *sk, uint32_t ack, bool ts_present,
                    uint32_t tsecr);
/** Derive a raw per-ACK Timestamp RTT sample without changing RTO state. */
bool tcp_rtt_sample_ack(const struct nsock *sk, bool ts_present,
                        uint32_t tsecr, uint32_t now_ms,
                        uint32_t *sample_ms);
/** Double the current data/FIN RTO and activate Karn suppression. */
void tcp_rtt_on_timeout(struct nsock *sk);
/** Activate Karn suppression for a fast retransmission without RTO backoff. */
void tcp_rtt_on_retransmit(struct nsock *sk);
/** Clear flight-local RTT state after all outstanding bytes are acknowledged.
 */
void tcp_rtt_on_flight_acked(struct nsock *sk);

#endif /* NETARCH_TCP_RTT_H */
