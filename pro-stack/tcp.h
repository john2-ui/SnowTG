/**
 * @file tcp.h
 * @brief TCP control block, table-driven state machine, and ingress/egress.
 *
 * The TCP-specific connection state lives in @ref tcp_stream, which is embedded
 * inside the unified @ref nsock (see socket.h) as @c nsock.u.tcp. The socket
 * itself owns the fd, rings, local address, and synchronization primitives, so
 * @ref tcp_stream only carries what is genuinely TCP-private: the peer 4-tuple,
 * the connection status, and the send/receive sequence numbers.
 *
 * The state machine is table-driven: @ref tcp_state_ops indexes one handler per
 * @ref TCP_STATUS, so adding a state or a transition is a one-line table edit
 * plus a handler function instead of touching a hand-written switch.
 *
 * Server-side passive open (LISTEN -> SYN_RECV -> ESTABLISHED) is implemented;
 * active open and teardown are stubbed (@ref tcp_state_drop) for now.
 */
#ifndef NETARCH_TCP_H
#define NETARCH_TCP_H

#include <rte_mbuf.h>
#include <rte_tcp.h>
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

        uint32_t sent_seq; /**< Next sequence number to send (host order). */
        uint32_t recv_ack; /**< Ack number tracked for the peer (host order). */

        TCP_STATUS status; /**< Current connection state. */
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
