/**
 * @file tcp.h
 * @brief Minimal TCP TCB table, passive open (server) 3-way handshake, and
 *        ingress/egress helpers.
 *
 * Currently only the server side of the handshake is implemented: an inbound
 * SYN creates a stream in LISTEN, a SYN+ACK is queued, and a matching ACK
 * moves the stream to ESTABLISHED. Socket API unification with UDP is TODO.
 */
#ifndef NETARCH_TCP_H
#define NETARCH_TCP_H

#include "net_addr.h"

#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_ring_core.h>
#include <rte_tcp.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/** Maximum number of TCP option words carried in a @ref tcp_fragment. */
#define TCP_MAX_OPTIONS 10

/**
 * @brief TCP connection state (subset of the classic TCP state machine).
 */
typedef enum _TCP_STATUS {
        TCP_STATUS_CLOSED,
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
} TCP_STATUS;

/**
 * @brief One TCP control block (TCB) / connection stream.
 *
 * For the current server-only path, @c src_* identifies the remote peer
 * (client) and @c dst_* identifies the local endpoint, both taken from the
 * first inbound segment. Addresses and ports are in network byte order;
 * sequence numbers are in host byte order.
 *
 * TODO: Merge with UDP @c local_addr once a shared socket layer exists.
 */
struct tcp_stream {
        int fd; /**< Socket fd placeholder; unused until sock API lands. */

        uint32_t src_ip;   /**< Remote IPv4 (network order). */
        uint32_t dst_ip;   /**< Local IPv4 (network order). */
        uint16_t src_port; /**< Remote TCP port (network order). */
        uint16_t dst_port; /**< Local TCP port (network order). */
        uint16_t proto;    /**< IP protocol (IPPROTO_TCP). */

        uint8_t local_mac[RTE_ETHER_ADDR_LEN]; /**< Local Ethernet address. */

        uint32_t sent_seq; /**< Next sequence number to send (host order). */
        uint32_t recv_ack; /**< Ack number tracked for the peer (host order). */

        TCP_STATUS status; /**< Current connection state. */

        struct rte_ring *recv_buf; /**< Inbound app payload fragments. */
        struct rte_ring *send_buf; /**< Outbound @ref tcp_fragment queue. */

        struct tcp_stream *prev; /**< Previous TCB in the table list. */
        struct tcp_stream *next; /**< Next TCB in the table list. */
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
 * @brief Global table of active TCP streams (TCB set).
 */
struct tcp_table {
        int count; /**< Number of streams currently in @c tcb_set. */
        struct tcp_stream *tcb_set; /**< Head of the TCB linked list. */
};

/**
 * @brief Get the singleton TCP table, creating it on first use.
 * @return Pointer to the initialized table (never NULL).
 */
struct tcp_table *tcp_table_instance(void);

/**
 * @brief Find an existing stream by the 4-tuple.
 *
 * @param src_ip   Remote IPv4 (network order).
 * @param dst_ip   Local IPv4 (network order).
 * @param src_port Remote TCP port (network order).
 * @param dst_port Local TCP port (network order).
 * @param proto    Unused; reserved for a unified socket lookup.
 * @return Matching stream, or NULL if none exists.
 */
struct tcp_stream *tcp_stream_search(uint32_t src_ip, uint32_t dst_ip,
                                     uint16_t src_port, uint16_t dst_port,
                                     uint8_t proto);

/**
 * @brief Allocate a new stream, seed an ISN, and insert it into the table.
 *
 * The stream starts in @c TCP_STATUS_LISTEN (server passive open).
 *
 * @param src_ip   Remote IPv4 (network order).
 * @param dst_ip   Local IPv4 (network order).
 * @param src_port Remote TCP port (network order).
 * @param dst_port Local TCP port (network order).
 * @param proto    Unused; reserved for a unified socket lookup.
 * @return Newly created stream (never NULL; aborts on allocation failure).
 */
struct tcp_stream *tcp_stream_create(uint32_t src_ip, uint32_t dst_ip,
                                     uint16_t src_port, uint16_t dst_port,
                                     uint8_t proto);

/**
 * @brief LISTEN-state handler: answer an inbound SYN with a queued SYN+ACK.
 *
 * On success the stream transitions to @c TCP_STATUS_SYN_RECV and a
 * @ref tcp_fragment is enqueued on @c stream->send_buf for @ref tcp_out.
 *
 * @param stream Connection in LISTEN.
 * @param tcphdr Inbound TCP header.
 * @return 0.
 */
int tcp_stream_handle_listen(struct tcp_stream *stream,
                             struct rte_tcp_hdr *tcphdr);

/**
 * @brief SYN_RECV-state handler: complete the handshake on a matching ACK.
 *
 * Transitions the stream to @c TCP_STATUS_ESTABLISHED when the peer ACK
 * acknowledges the local ISN (+1).
 *
 * @param stream Connection in SYN_RECV.
 * @param tcp_hdr Inbound TCP header.
 * @return 0.
 */
int tcp_stream_handle_syn_recv(struct tcp_stream *stream,
                               struct rte_tcp_hdr *tcp_hdr);

/**
 * @brief Dispatch one inbound TCP frame through the connection state machine.
 *
 * Creates a stream on first sight, learns the peer MAC into the ARP table,
 * then routes the segment to the handler for the current status. Does not
 * free @p mbuf; the caller owns that.
 *
 * @param mbuf Inbound Ethernet/IPv4/TCP frame.
 * @return 0.
 */
int tcp_handle(struct rte_mbuf *mbuf);

/**
 * @brief Drain pending outbound fragments from every active stream.
 *
 * For each stream in SYN_RECV or ESTABLISHED, dequeue one fragment, resolve
 * the peer MAC (emitting ARP if needed), build a packet, and enqueue it on
 * the NIC output ring.
 *
 * @param mp Mempool used for TCP and ARP packets.
 */
void tcp_out(struct rte_mempool *mp);

#endif /* NETARCH_TCP_H */
