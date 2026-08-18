/**
 * @file tcp.h
 * @brief TCP control block, table-driven state machine, and ingress/egress.
 *
 * The TCP-specific connection state lives in @ref tcp_stream, which is embedded
 * inside the unified @ref nsock (see socket.h) as @c nsock.u.tcp.  The nsock is
 * owned by one packet worker and named cross-lcore by a generation-checked
 * handle; integer fds remain application-side aliases.  tcp_stream therefore
 * carries only TCP-private state: peer tuple, sequence/window state, queues,
 * and the per-TCB owner-lcore timer.
 *
 * The state machine is table-driven: @ref tcp_state_ops indexes one handler per
 * @ref TCP_STATUS, so adding a state or a transition is a one-line table edit
 * plus a handler function instead of touching a hand-written switch.
 *
 * Passive open (LISTEN -> SYN_RECV -> ESTABLISHED), active open
 * (CLOSED -> SYN_SENT -> ESTABLISHED), and teardown (active FIN_WAIT_* /
 * TIME_WAIT / CLOSING, passive CLOSE_WAIT / LAST_ACK) are implemented.
 * tcp_ingress generates RFC 793 RST replies for unmatched non-RST segments
 * and validates received RSTs before tearing down a matching stream.
 */
#ifndef NETARCH_TCP_H
#define NETARCH_TCP_H

#include "list.h"
#include "owner_timer.h"
#include "rbtree.h"
#include "tcp_cc.h"
#include "tcp_sack.h"

#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nsock; /* Forward declaration; full definition in socket.h. */

/** Maximum number of TCP option words carried in a @ref tcp_fragment. */
#define TCP_MAX_OPTIONS 10
/** Largest Window Scale shift permitted by RFC 7323. */
#define TCP_WSCALE_MAX 14

/**
 * @brief TCP connection state (subset of the classic TCP state machine).
 *
 * @c TCP_STATUS_MAX is one past the last real state and is used to size the
 * state-handler table.
 */
typedef enum _TCP_STATUS {
        TCP_STATUS_CLOSED = 0,
        TCP_STATUS_LISTEN,
        TCP_STATUS_SYN_SENT,
        TCP_STATUS_SYN_RECV,
        TCP_STATUS_ESTABLISHED,
        TCP_STATUS_CLOSE_WAIT,
        TCP_STATUS_LAST_ACK,
        TCP_STATUS_TIME_WAIT,
        TCP_STATUS_CLOSING,
        TCP_STATUS_FIN_WAIT_1,
        TCP_STATUS_FIN_WAIT_2,
        TCP_STATUS_MAX,
} TCP_STATUS;

/** App-facing RX payload (no L2/L3/L4 headers). */
struct tcp_rx_blob {
        unsigned char *data;
        size_t len;
        size_t off;               /**< bytes already consumed by short recv */
        struct tcp_rx_blob *next; /**< Owner-local RX queue linkage. */
        void *storage;            /**< Owner-pool payload backing object. */
};

/**
 * One buffered out-of-order segment (payload copy).
 *
 * Indexed by an RB-tree for O(log n) insertion/overlap lookup and also linked
 * in sequence order (@c prev/@c next) for efficient drain from @c recv_ack.
 */
struct tcp_ofo_seg {
        uint32_t seq;
        uint32_t len;
        uint32_t data_off; /**< Trimmed prefix; data remains allocation base. */
        uint8_t has_fin;
        unsigned char *data;
        void *storage; /**< Owner-pool payload backing object. */

        /** O(log n) lookup index, keyed by @c seq. */
        struct rb_node rb;

        /* Sequence-ordered queue for O(1) drain from recv_ack. */
        struct tcp_ofo_seg *prev;
        struct tcp_ofo_seg *next;
};

struct tcp_tx_chunk {
        struct tcp_tx_chunk *next;
        uint8_t *data;
        void *storage;
        uint32_t seq; /**< Sequence number of data[0]. */
        uint16_t len;
        uint16_t off; /**< Acknowledged prefix within @c data. */
};

struct tcp_sndbuf {
        struct tcp_tx_chunk *head;
        struct tcp_tx_chunk *tail;
        uint32_t len;      /**< Bytes currently buffered (unacked+unsent). */
        uint32_t head_seq; /**< Sequence of the first buffered byte. */
};

/** Owner-local OFO counters and gauges sampled by the stack runtime. */
struct tcp_ofo_metrics {
        uint64_t segments_current;
        uint64_t segments_peak;
        uint64_t bytes_current;
        uint64_t bytes_peak;
        uint64_t accepted_segments;
        uint64_t accepted_bytes;
        uint64_t released_segments;
        uint64_t released_bytes;
        uint64_t reorder_distance_max;
        uint64_t drop_rcvbuf;
        uint64_t drop_seg_limit;
        uint64_t drop_byte_limit;
        uint64_t drop_owner_limit;
        uint64_t drop_alloc;
        uint64_t drop_pressure;
        uint64_t pressure_transitions;
        uint64_t pressure_active;
};

/** Return interval counters plus current gauges, then clear interval metrics. */
void tcp_ofo_metrics_take(struct tcp_ofo_metrics *out);
/** Reset one owner's OFO controller before the owner begins processing. */
void tcp_ofo_metrics_reset_owner(unsigned int lcore_id);

#ifdef TCP_TESTING
struct nsock;

/** Test the owner-match rule used when RSS prediction is unavailable. */
bool tcp_test_port_prediction_matches_owner(int predicted_queue,
                                            uint16_t owner_queue);

/**
 * Test-only OFO helpers. They expose internal reassembly operations while
 * leaving the production TCP API unchanged.
 */
void tcp_test_ofo_init(struct nsock *sk, uint32_t rcv_nxt,
                       uint32_t rcvbuf_size);
int tcp_test_ofo_insert(struct nsock *sk, uint32_t seq, const uint8_t *data,
                        uint32_t len, int has_fin);
struct tcp_ofo_seg *tcp_test_ofo_lower_bound(const struct nsock *sk,
                                             uint32_t seq);
void tcp_test_ofo_purge(struct nsock *sk);
/** Drain contiguous OFO data after a test advances RCV.NXT. */
void tcp_test_ofo_drain(struct nsock *sk);
/** Feed one payload/FIN range through the shared stream receive path. */
bool tcp_test_receive_stream_segment(struct nsock *sk, uint32_t seq,
                                     const uint8_t *data, uint32_t len,
                                     bool has_fin);
/** Reset metrics for the current test lcore. */
void tcp_test_ofo_metrics_reset(void);
/** Force pressure mode on or off without requiring production pool exhaustion. */
void tcp_test_ofo_force_pressure(bool enabled);
/** Restore production pressure decisions after a forced test state. */
void tcp_test_ofo_use_auto_pressure(void);
/** Override the owner byte budget; zero restores the production budget. */
void tcp_test_ofo_set_owner_limit(uint64_t bytes);
/** Force the next OFO descriptor/payload allocation to fail. */
void tcp_test_ofo_fail_next_alloc(void);
void tcp_test_update_snd_wnd(struct nsock *sk, uint32_t seg_seq,
                             uint32_t seg_ack, uint16_t seg_wnd);
/** Test-only initialization plus append through the lazy TX chunk path. */
int tcp_test_sndbuf_append(struct nsock *sk, uint32_t isn, const uint8_t *data,
                           size_t len);
/** Test-only ACK-prefix removal from the lazy TX chunk chain. */
void tcp_test_sndbuf_remove(struct nsock *sk, uint32_t len);
/** Test-only sequence lookup returning one contiguous chunk range. */
const uint8_t *tcp_test_sndbuf_peek(const struct nsock *sk, uint32_t seq,
                                    uint32_t *available);
/** Test-only destruction of every chunk in a send buffer. */
void tcp_test_sndbuf_free(struct nsock *sk);
/** Feed parsed SACK blocks into the sender scoreboard test seam. */
void tcp_test_sack_score_update(struct nsock *sk,
                                const struct tcp_sack_block *blocks,
                                uint8_t count);
/** Select the first retransmission hole using the current test TCB state. */
bool tcp_test_sack_schedule_retransmit(struct nsock *sk);
/** Reset all sender-side SACK soft state in tests. */
void tcp_test_sack_score_clear(struct nsock *sk);
/** Exercise duplicate-ACK/SACK recovery entry without external I/O. */
void tcp_test_process_peer_ack(struct nsock *sk, uint32_t ack,
                               bool classic_duplicate);
#endif

/**
 * @brief Initialize an empty TCP send buffer; payload chunks are lazy.
 *
 * @param sb  Send buffer to initialize.
 * @param isn Initial sequence number for @c head_seq.
 * @return 0 on success, -1 for an invalid destination.
 */
int tcp_sndbuf_init(struct tcp_sndbuf *sb, uint32_t isn);
/**
 * @brief Deallocate a TCP send buffer.
 *
 * @param sb Send buffer to deallocate.
 */
void tcp_sndbuf_free(struct tcp_sndbuf *sb);

/**
 * @brief TCP-private connection state, embedded in @ref nsock.u.tcp.
 *
 * @c remote_* identifies the peer; the local endpoint lives on the enclosing
 * @ref nsock (@c local_ip / @c local_port). Addresses and ports are in network
 * byte order; sequence numbers are in host byte order.
 */
struct tcp_stream {
        uint32_t remote_ip;   /**< Peer IPv4 (network order). */
        uint16_t remote_port; /**< Peer TCP port (network order). */
        uint16_t _pad;        /**< Keep the struct word-aligned. */

        /**
         * Listen backlog. Caps incomplete (SYN_RECV) handshakes plus
         * completed connections waiting in @c accept_queue.
         */
        uint32_t backlog;
        /** Count of child sockets still in SYN_RECV for this listener. */
        uint32_t syn_pending;
        /** ESTABLISHED children waiting for naccept(); owned by listener. */
        struct rte_ring *accept_queue;

        /** Parent listener; set on passive-open children, NULL otherwise. */
        struct nsock *listener;

        uint32_t sent_seq; /**< snd_nxt: next seq to send (host order). */
        uint32_t snd_una;  /**< Oldest unacknowledged seq (host order). */
        uint32_t recv_ack; /**< Next expected peer seq / our ACK field. */

        /**
         * Sliding-window buffer for @c nsend() payload (kept until ACK).
         * Control segments (SYN / SYN+ACK / FIN) go through the enclosing
         * socket's owner-local TX queue and are freed after TX; their RTO
         * rebuilds them onto that queue.
         */
        struct tcp_sndbuf sndbuf;

        /*
         * Receive-side flow control.  Command execution and packet ingress
         * both run on the owner, so rcvbuf_used is plain owner-local state.
         */
        uint32_t rcvbuf_size;
        uint32_t rcvbuf_used;
        /** A contiguous peer FIN has established the receive-side EOF. */
        bool peer_eof;
        struct tcp_rx_blob *rx_current; /**< Owner-held short-read blob. */
        /** Owner-local replacement for nsock.recv_buf when enabled. */
        struct tcp_rx_blob *rx_queue_head;
        struct tcp_rx_blob *rx_queue_tail;
        uint32_t rx_queue_count;

        /** Owner-local replacement for nsock.send_buf when enabled. */
        struct tcp_fragment *tx_queue_head;
        struct tcp_fragment *tx_queue_tail;
        uint32_t tx_queue_count;

        /*
         * Send-side flow control: most recently accepted peer advertised
         * window, decoded to unscaled bytes.
         */
        uint32_t snd_wnd;
        uint32_t snd_wl1;   /* SEG.SEQ of last accepted window update */
        uint32_t snd_wl2;   /* SEG.ACK of last accepted window update */
        bool snd_wnd_valid; /**< Whether snd_wl1/snd_wl2 contain an update. */

        /*
         * MSS that limits locally originated payload.  It is initialized to
         * TCP_DEFAULT_MSS and replaced by the peer's SYN MSS when present.
         */
        uint16_t snd_mss;
        /**
         * Scale announced in our SYN and used to encode post-SYN advertised
         * receive windows after Window Scale negotiation.
         */
        uint8_t rcv_wscale;
        /**
         * Scale announced by the peer in its SYN; used to decode its post-SYN
         * advertised receive windows after Window Scale negotiation.
         */
        uint8_t snd_wscale;
        /** True only after both endpoints have offered Window Scale. */
        bool wscale_ok;

        /** True only when both handshake SYNs carried Timestamp. */
        bool timestamps_ok;
        /** Latest peer TSval to echo as TSecr on the next outbound segment. */
        uint32_t ts_recent;
        /** True after @c ts_recent was seeded by a SYN or accepted data. */
        bool ts_recent_valid;
        /**
         * Local Timestamp-clock value when @c ts_recent was last updated.
         * PAWS invalidates a stale value after a sufficiently long idle period.
         */
        uint32_t ts_recent_age_ms;
        /** Most recent locally emitted TSval. */
        uint32_t ts_last_val;
        /** Timestamp fields decoded from the segment currently in ingress. */
        bool rx_timestamp_present;
        uint32_t rx_tsval;
        uint32_t rx_tsecr;

        /** RFC 2018 negotiation succeeds only when both SYNs carry kind 4. */
        bool sack_local_offered;
        bool sack_peer_permitted; /**< Peer SYN carried SACK-Permitted. */
        bool sack_permitted; /**< Both endpoints actually offered SACK. */
        /** Normalized OFO range containing the most recently received data. */
        bool sack_recent_valid;
        struct tcp_sack_block sack_recent; /**< Current first-block hint. */
        /** Recently emitted first blocks, newest first, revalidated on emit. */
        uint8_t sack_history_count;
        struct tcp_sack_block sack_history[TCP_SACK_MAX_BLOCKS]; /**< MRU. */
        /** One-shot RFC 2883 duplicate block for the next generated ACK. */
        bool dsack_pending;
        struct tcp_sack_block dsack_block; /**< Duplicate half-open range. */
        /** SACK blocks decoded from the segment currently in ingress. */
        uint8_t rx_sack_count;
        struct tcp_sack_block rx_sacks[TCP_SACK_MAX_BLOCKS]; /**< Host order. */

        /** RFC 6675 range scoreboard and loss-recovery state. */
        struct tcp_sack_state sack;
        /** Pluggable congestion-control state selected by config.h. */
        struct tcp_cc_state cc;
        /** A SYN retransmission reduces the post-handshake initial window. */
        bool syn_retransmitted;
        /** Close requested while congestion/window limits leave unsent data. */
        bool fin_deferred;
        /** SOL_SOCKET/SO_LINGER policy stored on this TCP socket. */
        bool linger_enabled;
        uint32_t linger_seconds;
        /** Absolute asynchronous close deadline; zero means none. */
        uint64_t close_deadline_cycles;

        /** RFC 6298 RTT estimators and current data/FIN RTO, in ms. */
        uint32_t srtt_ms;
        uint32_t rttvar_ms;
        uint32_t rto_ms;
        /** One unambiguous Timestamp RTTM probe for the current flight. */
        bool rtt_probe_valid;
        uint32_t rtt_probe_end_seq;
        uint32_t rtt_probe_tsval;
        /** Karn guard: suppress samples until the retransmitted flight ends. */
        bool rtt_retransmitting;

        TCP_STATUS status; /**< Current connection state. */

        /**
         * Per-TCB timer, multiplexed by @c status: SYN_SENT / SYN_RECV RTO,
         * data RTO, FIN RTO (FIN_WAIT_1 / LAST_ACK / CLOSING), TIME_WAIT 2MSL.
         */
        struct owner_timer timer;
        /** Retransmit count for the currently armed @c timer purpose. */
        uint8_t retries;

        /** RB-tree index for O(log n) OFO insertion and overlap lookup. */
        struct rb_root ofo_tree;

        /** Sequence-ordered DLL head/tail; head is drained at O(1). */
        struct tcp_ofo_seg *ofo;
        struct tcp_ofo_seg *ofo_tail;

        /** OFO resource accounting, owned exclusively by packet worker. */
        uint16_t ofo_count;
        uint32_t ofo_bytes;
        /** Largest forward sequence distance in the current OFO episode. */
        uint32_t ofo_reorder_distance_peak;
};

/**
 * @brief Host-side description of one outbound TCP segment.
 *
 * Field layout mirrors @c rte_tcp_hdr. Ports stay in network byte order;
 * @c sent_seq, @c recv_ack, and @c rx_win are host byte order and converted
 * when the on-wire header is encoded.
 */
struct tcp_fragment {
        uint16_t src_port; /**< Source port (network order). */
        uint16_t dst_port; /**< Destination port (network order). */
        uint32_t sent_seq; /**< Sequence number (host order). */
        uint32_t recv_ack; /**< Acknowledgment number (host order). */
        uint8_t data_off;  /**< Data offset byte (upper nibble = hdr words). */
        uint8_t tcp_flags; /**< SYN/ACK/FIN/... flag bits. */
        /**
         * Encoded 16-bit receive window sent on the wire (host order).
         * The caller has already applied any negotiated Window Scale.
         */
        uint16_t rx_win;
        uint16_t cksum;   /**< Unused before encode; filled on the wire. */
        uint16_t tcp_urp; /**< Urgent pointer. */

        int opt_len; /**< Option length in 32-bit words. */
        uint32_t options[TCP_MAX_OPTIONS]; /**< TCP options payload. */

        unsigned char *payload;    /**< Optional payload bytes (may be NULL). */
        size_t payload_len;        /**< Payload length in bytes. */
        struct tcp_fragment *next; /**< Owner-local TX queue linkage. */
};

/**
 * @brief Find an existing TCP connection by the 4-tuple.
 *
 * Scans the unified socket list for a TCP socket whose peer and local tuple
 * match. The local half is read from the enclosing @ref nsock.
 *
 * @param remote_ip   Peer IPv4 (network order).
 * @param local_ip    Local IPv4 (network order).
 * @param remote_port Peer TCP port (network order).
 * @param local_port  Local TCP port (network order).
 * @return Matching socket, or NULL if none exists.
 */
struct nsock *tcp_stream_search(uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port);

/**
 * @brief Allocate a new TCP child control block for a passive-open handshake.
 *
 * Creates an owner-adopted socket in SYN_RECV without publishing an
 * application handle.  ACCEPT publishes the handle and allocates an fd only
 * after the handshake, so a SYN flood cannot exhaust the fd table.
 *
 * @param remote_ip   Peer IPv4 (network order).
 * @param local_ip    Local IPv4 (network order).
 * @param remote_port Peer TCP port (network order).
 * @param local_port  Local TCP port (network order).
 * @return Newly created socket, or NULL on allocation failure.
 */
struct nsock *tcp_stream_create(uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port);

/** Initialize the embedded owner timer for a newly allocated TCP socket. */
void tcp_timer_init(struct nsock *sk);
/** Force terminal local cleanup from an owner-local control path. */
void tcp_force_abort(struct nsock *sk, int error, const char *reason);

/**
 * @brief Dequeue one established passive-open child without blocking.
 *
 * This owner-side primitive returns EAGAIN when the accept queue is empty.
 * The socket command layer parks a blocking ACCEPT request and retries it when
 * the handshake path reports a newly completed child.
 */
struct nsock *tcp_accept_owned(struct nsock *listener);

/**
 * @brief Transition a TCP socket to a new status with uniform logging.
 *
 * Centralizing the transition makes the state machine easy to audit and
 * extend: every status change goes through this one helper.
 */
void tcp_stream_set_status(struct nsock *sk, TCP_STATUS new_status);

/**
 * @brief Dispatch one inbound TCP frame through the connection state machine.
 *
 * Parses the frame, looks up an existing TCB by 4-tuple, or (for a bare SYN)
 * a listening socket on the destination port. Learns the peer MAC into the ARP
 * table, then routes the segment to the appropriate handler. Always consumes
 * @p mbuf (see sock_ops.h lifecycle contract).
 *
 * @return 0.
 */
int tcp_ingress(struct rte_mbuf *mbuf);

/**
 * @brief Drain pending outbound fragments from one TCP socket to the NIC.
 *
 * Dequeues one @ref tcp_fragment, resolves the peer MAC (emitting ARP if
 * needed), builds a packet, and enqueues it on the NIC output ring.
 *
 * @return A @ref sock_tx_flush_result scheduling result: idle, retry, or
 *         ARP-wait.
 */
int tcp_tx_flush(struct nsock *sk, struct rte_mempool *mp);

/** Initial advertised window: 10 * default MSS. */
#define TCP_INITIAL_WINDOW_SIZE 14600

#endif /* NETARCH_TCP_H */
