/**
 * @file tcp_rtt.c
 * @brief RFC 6298 RTT estimation and data/FIN retransmission timeout state.
 */
#include "tcp_rtt.h"

#include "config.h"
#include "socket.h"
#include "tcp_options.h"

/**
 * @brief Bound an RTO to the configured RFC 6298 operating range.
 *
 * A 64-bit input prevents overflow before the upper limit is applied during
 * exponential retransmission backoff.
 */
static uint32_t tcp_rtt_clamp(uint64_t rto) {
        if (rto < TCP_RTO_MIN_MS)
                return TCP_RTO_MIN_MS;
        if (rto > TCP_RTO_MAX_MS)
                return TCP_RTO_MAX_MS;
        return (uint32_t)rto;
}

/**
 * @brief Reset data/FIN RTT state for a newly established connection.
 *
 * SYN/SYN+ACK keep their separate fixed retransmission timer.  Data and FIN
 * start at the RFC 6298 initial RTO until a Timestamp RTT sample arrives.
 */
void tcp_rtt_reset(struct nsock *sk) {
        sk->u.tcp.srtt_ms = 0;
        sk->u.tcp.rttvar_ms = 0;
        sk->u.tcp.rto_ms = TCP_RTO_INITIAL_MS;
        sk->u.tcp.rtt_probe_valid = false;
        sk->u.tcp.rtt_probe_end_seq = 0;
        sk->u.tcp.rtt_probe_tsval = 0;
        sk->u.tcp.rtt_retransmitting = false;
}

/**
 * @brief Save one Timestamp RTT measurement probe for the current flight.
 * @param end_seq First sequence number beyond the transmitted payload.
 * @param tsval TSval encoded on that payload segment.
 *
 * Only the oldest new data segment in a flight is measured.  A later segment
 * does not replace it, and retransmitted flights are deliberately excluded by
 * the Karn guard.
 */
void tcp_rtt_note_xmit(struct nsock *sk, uint32_t end_seq, uint32_t tsval) {
        if (!sk->u.tcp.timestamps_ok || sk->u.tcp.rtt_retransmitting ||
            sk->u.tcp.rtt_probe_valid)
                return;

        sk->u.tcp.rtt_probe_end_seq = end_seq;
        sk->u.tcp.rtt_probe_tsval = tsval;
        sk->u.tcp.rtt_probe_valid = true;
}

/**
 * @brief Consume an advancing ACK as a possible Timestamp RTT measurement.
 * @return True only if the ACK produced a new RFC 6298 RTT sample.
 *
 * TSecr identifies the exact TSval being acknowledged.  The ACK must also
 * cover the probe payload, so duplicate ACKs, partial unrelated ACKs, and
 * echoes from other segments cannot update the estimator.
 */
bool tcp_rtt_on_ack(struct nsock *sk, uint32_t ack, bool ts_present,
                    uint32_t tsecr) {
        uint32_t sample;
        uint32_t delta;
        uint64_t rto;

        if (!sk->u.tcp.timestamps_ok || !ts_present ||
            !sk->u.tcp.rtt_probe_valid || sk->u.tcp.rtt_retransmitting ||
            tsecr != sk->u.tcp.rtt_probe_tsval ||
            (int32_t)(ack - sk->u.tcp.rtt_probe_end_seq) < 0)
                return false;

        /*
         * Both values use the modulo-2^32 millisecond Timestamp clock. Unsigned
         * subtraction intentionally retains the correct small delta at wrap.
         */
        sample = tcp_options_now_ms() - tsecr;
        if (sample == 0)
                sample = 1;

        if (sk->u.tcp.srtt_ms == 0) {
                /* RFC 6298 §2.2: first valid RTT sample. */
                sk->u.tcp.srtt_ms = sample;
                sk->u.tcp.rttvar_ms = sample / 2;
        } else {
                /* RFC 6298 §2.3: beta=1/4, alpha=1/8, integer form. */
                delta = sk->u.tcp.srtt_ms > sample ? sk->u.tcp.srtt_ms - sample
                                                   : sample - sk->u.tcp.srtt_ms;
                sk->u.tcp.rttvar_ms = (3 * sk->u.tcp.rttvar_ms + delta) / 4;
                sk->u.tcp.srtt_ms = (7 * sk->u.tcp.srtt_ms + sample) / 8;
        }

        /* G is one millisecond; clamping also enforces the configured floor. */
        rto = (uint64_t)sk->u.tcp.srtt_ms + 4 * sk->u.tcp.rttvar_ms;
        sk->u.tcp.rto_ms = tcp_rtt_clamp(rto);
        sk->u.tcp.rtt_probe_valid = false;
        return true;
}

/**
 * @brief Apply RFC 6298 retransmission backoff and suppress ambiguous samples.
 *
 * Once a flight was retransmitted, an ACK cannot safely identify the original
 * transmission without a fuller per-segment timestamp scoreboard.
 */
void tcp_rtt_on_timeout(struct nsock *sk) {
        sk->u.tcp.rto_ms = tcp_rtt_clamp((uint64_t)sk->u.tcp.rto_ms * 2);
        sk->u.tcp.rtt_probe_valid = false;
        sk->u.tcp.rtt_retransmitting = true;
}

/**
 * @brief Activate Karn suppression for fast or SACK retransmission.
 *
 * Unlike @ref tcp_rtt_on_timeout, this path deliberately leaves the current
 * RTO unchanged because no retransmission timer expired.
 */
void tcp_rtt_on_retransmit(struct nsock *sk) {
        sk->u.tcp.rtt_probe_valid = false;
        sk->u.tcp.rtt_retransmitting = true;
}

/**
 * @brief End Karn suppression after every segment in the flight is ACKed.
 */
void tcp_rtt_on_flight_acked(struct nsock *sk) {
        sk->u.tcp.rtt_probe_valid = false;
        sk->u.tcp.rtt_retransmitting = false;
}
