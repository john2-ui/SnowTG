/**
 * @file tcp.h
 * @brief TCP control block, table-driven state machine, and ingress/egress.
 *
 * The TCP-specific connection state lives in @ref tcp_stream, which is embedded
 * inside the unified @ref nsock (see socket.h) as @c nsock.u.tcp. The socket
 * itself owns the fd, rings, local address, and synchronization primitives, so
 * @ref tcp_stream only carries what is genuinely TCP-private: the peer 4-tuple,
 * the connection status, the send/receive sequence numbers, and the per-TCB
 * timer / retry counters (SYN / SYN+ACK / data / FIN RTO and TIME_WAIT 2MSL).
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
#include "rbtree.h"

#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_timer.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nsock; /* Forward declaration; full definition in socket.h. */

/** Maximum number of TCP option words carried in a @ref tcp_fragment. */
#define TCP_MAX_OPTIONS 10

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
        size_t off; /**< bytes already consumed by short recv */
};

/**
 * One buffered out-of-order segment (payload copy).
 *
 * Chained as a seq-sorted doubly-linked list today (@c prev/@c next).
 * TODO: add an rb-tree node (Linux ofo style) for O(log n) insert while
 * keeping the list for in-order drain from @c recv_ack.
 */
struct tcp_ofo_seg {
        uint32_t seq;
        uint32_t len;
        uint8_t has_fin;
        unsigned char *data;

        /*O(log n) lookup index, keyed by seq*/
        struct rb_node rb;

        /* Sequence-ordered queue for O(1) drain from recv_ack. */
        struct tcp_ofo_seg *prev;
        struct tcp_ofo_seg *next;
};

struct tcp_sndbuf {
        uint8_t *data;     /**< Contiguous payload storage. */
        uint32_t size;     /**< Capacity (TCP_SNDBUF_SIZE). */
        uint32_t head_off; /**< Offset of first buffered byte. */
        uint32_t len;      /**< Bytes currently buffered (unacked+unsent). */
        uint32_t head_seq; /**< Seq of data[head_off]; tracks snd_una. */
};

/**
 * @brief Allocate and initialize a TCP send buffer.
 *
 * @param sb  Send buffer to initialize.
 * @param isn Initial sequence number for @c head_seq.
 * @return 0 on success, -1 on allocation failure.
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
         * Control segments (SYN / SYN+ACK / FIN) go on @c nsock.send_buf and
         * are freed after TX; their RTO rebuilds them onto send_buf.
         */
        struct tcp_sndbuf sndbuf;

        /*
         * Receive-side flow control. Only the packet worker modifies
         * rcvbuf_used; the application accumulates consumed bytes for the
         * worker in rx_consumed.
         */
        uint32_t rcvbuf_size;
        uint32_t rcvbuf_used;
        atomic_uint rx_consumed;
        atomic_bool rx_event_pending;
        struct tcp_rx_blob *rx_current; /**< App-owned short-read blob. */

        /* Send-side flow control: most recently accepted peer advertised
         * window. */
        uint32_t snd_wnd;
        uint32_t snd_wl1; /* SEG.SEQ of last accepted window update */
        uint32_t snd_wl2; /* SEG.ACK of last accepted window update */

        TCP_STATUS status; /**< Current connection state. */

        /**
         * Per-TCB timer, multiplexed by @c status: SYN_SENT / SYN_RECV RTO,
         * data RTO, FIN RTO (FIN_WAIT_1 / LAST_ACK / CLOSING), TIME_WAIT 2MSL.
         */
        struct rte_timer timer;
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
        uint16_t rx_win;   /**< Receive window (host order). */
        uint16_t cksum;    /**< Unused before encode; filled on the wire. */
        uint16_t tcp_urp;  /**< Urgent pointer. */

        int opt_len; /**< Option length in 32-bit words. */
        uint32_t options[TCP_MAX_OPTIONS]; /**< TCP options payload. */

        unsigned char *payload; /**< Optional payload bytes (may be NULL). */
        size_t payload_len;     /**< Payload length in bytes. */
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
 * Creates a socket in @c TCP_STATUS_SYN_RECV with @c fd == -1. A real fd is
 * assigned later in @ref tcp_accept once the handshake completes, so a SYN
 * flood cannot exhaust the process fd table.
 *
 * @param remote_ip   Peer IPv4 (network order).
 * @param local_ip    Local IPv4 (network order).
 * @param remote_port Peer TCP port (network order).
 * @param local_port  Local TCP port (network order).
 * @return Newly created socket, or NULL on allocation failure.
 */
struct nsock *tcp_stream_create(uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port);

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
 * @brief Process application receive-progress notifications on the worker.
 *
 * Applications enqueue one coalesced notification after consuming payload.
 * This function is called only by the packet worker and is the sole path that
 * updates TCP receive accounting, drains OFO data, and sends window updates.
 */
void tcp_process_app_events(void);

/**
 * @brief Drain pending outbound fragments from one TCP socket to the NIC.
 *
 * Dequeues one @ref tcp_fragment, resolves the peer MAC (emitting ARP if
 * needed), builds a packet, and enqueues it on the NIC output ring.
 *
 * @return 0 after attempting one dequeue.
 */
int tcp_tx_flush(struct nsock *sk, struct rte_mempool *mp);

/** Initial advertised window: 10 * default MSS. */
#define TCP_INITIAL_WINDOW_SIZE 14600

#endif /* NETARCH_TCP_H */
