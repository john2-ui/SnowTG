/**
 * @file tcp_cc.h
 * @brief Pluggable TCP congestion-control interface and shared state.
 */
#ifndef NETARCH_TCP_CC_H
#define NETARCH_TCP_CC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tcp_stream;
struct tcp_sack_block;

/** Fixed per-stream storage available to a congestion-control algorithm. */
#define TCP_CC_PRIV_SIZE 64U

/** Cause passed to the congestion-control loss callback. */
enum tcp_cc_loss_reason {
        TCP_CC_LOSS_SACK = 0, /**< RFC 6675 declared data lost. */
        TCP_CC_LOSS_DUPACK,   /**< Classic three-duplicate-ACK loss. */
        TCP_CC_LOSS_RTO,      /**< Retransmission timer expired. */
};

/** Atomic cumulative-ACK/SACK event delivered under the socket mutex. */
struct tcp_cc_ack_event {
        uint32_t acked_bytes; /**< Newly cumulatively ACKed payload bytes. */
        uint32_t newly_sacked_bytes; /**< Newly selectively ACKed bytes. */
        uint32_t flight_size; /**< Payload flight before applying this ACK. */
        bool in_recovery;     /**< ACK belongs to entry/active recovery. */
};

/** Loss information supplied to fast-loss and RTO callbacks. */
struct tcp_cc_loss_event {
        enum tcp_cc_loss_reason reason; /**< Detection mechanism. */
        uint32_t flight_size; /**< Outstanding payload when loss was found. */
        uint32_t recovery_point; /**< Fixed half-open recovery boundary. */
        bool first_rto_for_seq; /**< False on repeated RTO of the same UNA. */
};

/**
 * @brief Congestion-control algorithm callback table.
 *
 * Every callback runs while the owning socket mutex is held.  Callbacks may
 * update only congestion-control state; the TCP recovery module owns packet
 * selection and transmission.  NULL optional callbacks are ignored.
 */
struct tcp_cc_ops {
        const char *name; /**< Stable diagnostic algorithm name. */
        size_t priv_size; /**< Bytes used in @ref tcp_cc_state.priv. */
        /** Initialize common/private state after MSS negotiation. */
        void (*init)(struct tcp_stream *tp, bool syn_retransmitted);
        /** Reset an existing stream without replacing the selected ops. */
        void (*reset)(struct tcp_stream *tp);
        /** Consume one atomic ACK/SACK event. */
        void (*on_ack)(struct tcp_stream *tp,
                       const struct tcp_cc_ack_event *event);
        /** Observe a successfully queued data or retransmission packet. */
        void (*on_packet_sent)(struct tcp_stream *tp, uint32_t bytes,
                               bool retransmission);
        /** React to loss detected without an RTO. */
        void (*on_loss)(struct tcp_stream *tp,
                        const struct tcp_cc_loss_event *event);
        /** Deflate or otherwise finalize state after recovery. */
        void (*on_recovery_exit)(struct tcp_stream *tp);
        /** React to retransmission timeout independently of fast loss. */
        void (*on_rto)(struct tcp_stream *tp,
                       const struct tcp_cc_loss_event *event);
        /** Apply the algorithm's restart-after-idle policy. */
        void (*on_idle_restart)(struct tcp_stream *tp);
        /** Observe a D-SACK and whether it covers locally retransmitted data. */
        void (*on_dsack)(struct tcp_stream *tp,
                         const struct tcp_sack_block *block,
                         bool covers_retransmission);
};

/** Algorithm-independent congestion-control state stored in every TCB. */
struct tcp_cc_state {
        const struct tcp_cc_ops *ops; /**< Selected algorithm callbacks. */
        uint32_t cwnd;                /**< Congestion window in bytes. */
        uint32_t ssthresh;            /**< Slow-start threshold in bytes. */
        uint32_t initial_window;      /**< MSS-derived restart window. */
        uint32_t ca_acked; /**< Byte counter for additive increase. */
        uint32_t rto_loss_seq; /**< SND.UNA associated with the last RTO. */
        bool rto_loss_valid; /**< Whether @c rto_loss_seq is initialized. */
        uint32_t last_data_tx_ms; /**< Timestamp for restart-after-idle. */
        uint8_t priv[TCP_CC_PRIV_SIZE]; /**< Algorithm-private fixed storage. */
};

/** Select and initialize @p ops when its private state fits the TCB. */
void tcp_cc_set_ops(struct tcp_stream *tp, const struct tcp_cc_ops *ops,
                    bool syn_retransmitted);
/** Select the built-in RFC 5681 Reno implementation. */
void tcp_cc_use_reno(struct tcp_stream *tp, bool syn_retransmitted);
/** Reset the currently selected congestion-control implementation. */
void tcp_cc_reset(struct tcp_stream *tp);
/** Dispatch an atomic ACK/SACK event to the selected algorithm. */
void tcp_cc_on_ack(struct tcp_stream *tp,
                   const struct tcp_cc_ack_event *event);
/** Record and dispatch a successfully queued payload transmission. */
void tcp_cc_on_packet_sent(struct tcp_stream *tp, uint32_t bytes,
                           bool retransmission, uint32_t now_ms);
/** Dispatch non-RTO loss detection to the selected algorithm. */
void tcp_cc_on_loss(struct tcp_stream *tp,
                    const struct tcp_cc_loss_event *event);
/** Notify the selected algorithm that TCP recovery has ended. */
void tcp_cc_on_recovery_exit(struct tcp_stream *tp);
/** Dispatch a retransmission timeout to the selected algorithm. */
void tcp_cc_on_rto(struct tcp_stream *tp,
                   const struct tcp_cc_loss_event *event);
/** Apply restart-after-idle when no data was sent for more than one RTO. */
void tcp_cc_on_idle_restart(struct tcp_stream *tp, uint32_t now_ms,
                            uint32_t rto_ms);
/** Dispatch one classified RFC 2883 D-SACK event. */
void tcp_cc_on_dsack(struct tcp_stream *tp,
                     const struct tcp_sack_block *block,
                     bool covers_retransmission);
/** Return the active congestion window, or unlimited if no ops are selected. */
uint32_t tcp_cc_send_window(const struct tcp_stream *tp);

/** Built-in minimal RFC 5681 Reno callback table. */
extern const struct tcp_cc_ops tcp_reno_ops;

#endif /* NETARCH_TCP_CC_H */
