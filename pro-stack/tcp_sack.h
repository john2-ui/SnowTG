/**
 * @file tcp_sack.h
 * @brief RFC 2018/6675 sender scoreboard and loss-recovery interfaces.
 */
#ifndef NETARCH_TCP_SACK_H
#define NETARCH_TCP_SACK_H

#include <stdbool.h>
#include <stdint.h>

struct tcp_stream;

/** Maximum SACK blocks representable in one TCP option. */
#define TCP_SACK_MAX_BLOCKS 4U
/** Per-connection cap shared by SACKED and RETRANSMITTED ranges. */
#define TCP_SACK_SCORE_MAX_RANGES 128U
/** RFC duplicate-ACK/SACK loss threshold. */
#define TCP_SACK_DUP_THRESH 3U

/** Host-order half-open TCP sequence interval [left, right). */
struct tcp_sack_block {
        uint32_t left;  /**< First sequence number in the block. */
        uint32_t right; /**< First sequence number after the block. */
};

/** Meaning attached to one sender scoreboard range. */
enum tcp_sack_range_flags {
        TCP_SACK_RANGE_SACKED = 1U << 0, /**< Peer selectively ACKed it. */
        TCP_SACK_RANGE_RETRANSMITTED =
            1U << 1, /**< It was transmitted again during recovery. */
};

/** One sorted, non-overlapping, pool-backed scoreboard interval. */
struct tcp_sack_range {
        struct tcp_sack_range *next; /**< Next range in sequence order. */
        uint32_t left;               /**< Inclusive left edge. */
        uint32_t right;              /**< Exclusive right edge. */
        uint8_t flags;               /**< @ref tcp_sack_range_flags bitmap. */
};

/** Mutually exclusive sender loss-recovery states. */
enum tcp_recovery_mode {
        TCP_RECOVERY_NORMAL = 0, /**< No loss recovery is active. */
        TCP_RECOVERY_SACK,       /**< RFC 6675 SACK fast recovery. */
        TCP_RECOVERY_RTO,        /**< Recovery started by an RTO. */
        TCP_RECOVERY_CLASSIC_RENO, /**< Non-SACK Reno fast recovery. */
};

/** Type of work selected by RFC 6675 NextSeg. */
enum tcp_recovery_tx_kind {
        TCP_RECOVERY_TX_NONE = 0, /**< No recovery transmission pending. */
        TCP_RECOVERY_TX_RETRANSMIT, /**< Read an old range from sndbuf. */
        TCP_RECOVERY_TX_NEW_DATA, /**< Send data at the current SND.NXT. */
};

/**
 * @brief Recovery transmission selected but not yet committed.
 *
 * The descriptor survives transient ARP, mbuf, and TX-ring failures.  Sender
 * recovery state is advanced only after the packet reaches the output ring.
 */
struct tcp_recovery_candidate {
        enum tcp_recovery_tx_kind kind; /**< Retransmit, new data, or none. */
        uint8_t nextseg_rule; /**< RFC 6675 NextSeg rule 1 through 4. */
        bool rescue;          /**< True for the one permitted rescue segment. */
        uint32_t seq;         /**< Inclusive sequence edge. */
        uint32_t end;         /**< Exclusive sequence edge, at most one SMSS. */
};

/** Per-stream RFC 6675 scoreboard and recovery variables. */
struct tcp_sack_state {
        /** Sorted ranges reported by currently valid peer SACK blocks. */
        struct tcp_sack_range *sacked;
        /** Sorted ranges retransmitted during current or recent recovery. */
        struct tcp_sack_range *retransmitted;
        /** Total allocated nodes across both lists. */
        uint16_t range_count;
        /** Pool/cap exhaustion disabled SACK optimization for this flight. */
        bool degraded;
        /** Cumulative ACK boundary that ends the degraded flight. */
        uint32_t degraded_until;

        enum tcp_recovery_mode mode; /**< Current loss-recovery mode. */
        uint32_t high_data;      /**< Half-open HighData / SND.NXT payload. */
        uint32_t high_rxt;       /**< Half-open right edge retransmitted. */
        uint32_t rescue_rxt;     /**< Half-open rescue guard. */
        uint32_t recovery_point; /**< Half-open recovery boundary. */
        uint32_t pipe;           /**< RFC 6675 estimate of bytes in network. */
        uint8_t dup_acks;        /**< New-SACK or classic duplicate ACK count. */
        bool limited_transmit;   /**< Permit one RFC 6675 limited transmit. */
        struct tcp_recovery_candidate pending; /**< Uncommitted TX choice. */
};

/** Result of atomically applying one ACK's SACK option to the scoreboard. */
struct tcp_sack_ack_result {
        uint32_t newly_sacked_bytes; /**< Bytes not covered before this ACK. */
        bool new_sack_information;   /**< At least one new byte was SACKed. */
        bool dsack_valid;            /**< First block was classified D-SACK. */
        struct tcp_sack_block dsack; /**< Classified duplicate interval. */
        bool dsack_covers_retransmission; /**< Duplicate overlaps retransmit. */
        bool recovery_exited; /**< ACK reached the fixed RecoveryPoint. */
};

/** Initialize an empty scoreboard at @p initial_seq. */
void tcp_sack_state_init(struct tcp_stream *tp, uint32_t initial_seq);
/** Release all range nodes and reinitialize at @p initial_seq. */
void tcp_sack_state_reset(struct tcp_stream *tp, uint32_t initial_seq);
/** Clip both range lists to the current payload flight. */
void tcp_sack_trim(struct tcp_stream *tp, uint32_t flight_end);
/**
 * @brief Apply and merge the SACK information carried by one ACK.
 * @param packet_ack ACK field used to recognize RFC 2883 D-SACK.
 * @param blocks Host-order blocks parsed from the current packet.
 * @param count Number of entries in @p blocks.
 * @param flight_end Exclusive right edge of already transmitted payload.
 * @return New-SACK, D-SACK, and recovery-exit events for ACK/CC processing.
 */
struct tcp_sack_ack_result
tcp_sack_update(struct tcp_stream *tp, uint32_t packet_ack,
                const struct tcp_sack_block *blocks, uint8_t count,
                uint32_t flight_end);
/** Return whether @p seq satisfies RFC 6675 IsLost. */
bool tcp_sack_is_lost(const struct tcp_stream *tp, uint32_t seq);
/** Recompute and store RFC 6675 Pipe for the current payload flight. */
uint32_t tcp_sack_set_pipe(struct tcp_stream *tp, uint32_t flight_end);
/** Enter the requested recovery mode and select the first retransmission. */
void tcp_sack_enter_recovery(struct tcp_stream *tp,
                             enum tcp_recovery_mode mode,
                             uint32_t flight_end);
/** Clear renegable SACK state and start explicit-sequence RTO recovery. */
void tcp_sack_on_rto(struct tcp_stream *tp, uint32_t flight_end);
/**
 * @brief Run RFC 6675 NextSeg and retain one transmission candidate.
 * @return True when a previous or newly selected candidate is pending.
 */
bool tcp_sack_schedule_next(struct tcp_stream *tp, uint32_t sndbuf_end,
                            bool allow_new_data);
/** Commit a successful candidate transmission through @p sent_end. */
void tcp_sack_commit_candidate(struct tcp_stream *tp, uint32_t sent_end);
/** Discard an invalid candidate without changing recovery variables. */
void tcp_sack_cancel_candidate(struct tcp_stream *tp);
/** Advance HighData after successfully transmitting new payload. */
void tcp_sack_note_new_data(struct tcp_stream *tp, uint32_t end_seq);
/** Return the number of currently SACKED scoreboard intervals. */
uint16_t tcp_sack_score_count(const struct tcp_stream *tp);

#endif /* NETARCH_TCP_SACK_H */
