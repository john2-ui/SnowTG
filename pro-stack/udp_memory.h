#ifndef NETARCH_UDP_MEMORY_H
#define NETARCH_UDP_MEMORY_H

/**
 * @file udp_memory.h
 * @brief Owner-local lazy metadata for traffic-generator UDP receive queues.
 *
 * UDP payload ownership remains with the DPDK mbuf received from the NIC.
 * This domain only supplies queue metadata when a local UDP socket actually
 * retains a datagram, so an idle socket has no queue allocation.
 */

#include <stdint.h>

struct rte_mempool;
struct rte_mbuf;

/**
 * @brief Lazy metadata for one datagram retained by an owner-local UDP socket.
 *
 * The packet bytes and peer tuple remain in @c mbuf.  Only this small node is
 * acquired from the current owner's UDP memory domain while the datagram is
 * waiting for the traffic-generator reactor.
 */
struct udp_rx_node {
        struct rte_mbuf *mbuf;
        struct udp_rx_node *next;
};

/** Per-owner pool for local UDP receive queue metadata. */
struct udp_owner_memory {
        struct rte_mempool *rx_nodes;
        uint32_t capacity;
        uint32_t alloc_fail;
        uint32_t peak_in_use;
        uint64_t queue_drops;
        uint16_t lcore_id;
};

/** Read-only counters used by owner-local diagnostics. */
struct udp_memory_snapshot {
        uint32_t capacity;
        uint32_t available;
        uint32_t alloc_fail;
        uint32_t peak_in_use;
        uint64_t queue_drops;
};

/** Create the owner-local UDP queue-node pool. */
int udp_owner_memory_init(struct udp_owner_memory *memory,
                          unsigned int lcore_id);
/** Destroy the pool after all owner-local UDP sockets have been drained. */
void udp_owner_memory_fini(struct udp_owner_memory *memory);
/** Copy current availability and drop/allocation counters. */
void udp_owner_memory_snapshot(const struct udp_owner_memory *memory,
                               struct udp_memory_snapshot *snapshot);

/** Acquire a zeroed queue node for one retained datagram. */
struct udp_rx_node *udp_memory_rx_node_alloc(struct udp_owner_memory *memory);
/** Return a queue node after its mbuf has been released by the caller. */
void udp_memory_rx_node_free(struct udp_owner_memory *memory,
                             struct udp_rx_node *node);
/** Record a datagram dropped because local queue resources were unavailable. */
void udp_memory_record_queue_drop(struct udp_owner_memory *memory);

#endif /* NETARCH_UDP_MEMORY_H */
