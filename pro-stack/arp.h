/**
 * @file arp.h
 * @brief ARP table plus ARP packet build/handle helpers.
 *
 * The table state lives in arp.c (not in this header) so that including the
 * header from several translation units no longer creates independent copies
 * of the table.
 */
#ifndef NETARCH_ARP_H
#define NETARCH_ARP_H

#include <rte_arp.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Neighbour reachability state used by the demand-driven ARP resolver.
 */
enum arp_state {
        ARP_STATE_FREE = 0,
        ARP_STATE_INCOMPLETE,
        ARP_STATE_REACHABLE,
        ARP_STATE_FAILED,
};

/**
 * @brief One fixed-capacity IPv4 neighbour entry.
 */
struct arp_entry {
        uint32_t ip; /**< IPv4 address in network byte order. */
        uint8_t hwaddr[RTE_ETHER_ADDR_LEN]; /**< Associated Ethernet address. */
        enum arp_state state;
        uint8_t probe_count;
        uint64_t confirmed_at;  /**< Last ARP or IPv4 traffic confirmation. */
        uint64_t last_probe_at; /**< Last ARP request emission attempt. */
        uint64_t last_used_at; /**< Last successful lookup, for LRU eviction. */
};

/**
 * @brief O(1) IPv4 neighbour cache backed by a DPDK hash table.
 */
struct arp_table {
        struct rte_hash *hash;
        struct arp_entry *entries;
        uint32_t capacity;
        uint32_t count;
};

/**
 * @brief Get the singleton ARP table, creating it on first use.
 * @return Pointer to the initialized ARP table (never NULL).
 */
struct arp_table *arp_table_instance(void);

/**
 * @brief Resolve an IPv4 neighbour and enqueue a rate-limited ARP request.
 *
 * Called only by the packet worker.  A cache miss creates an INCOMPLETE
 * entry and sends at most one request per probe interval; callers retain their
 * existing socket-side pending data and retry on later flushes.
 *
 * @param mp Mempool used to allocate a request mbuf.
 * @param out TX ring for an ARP request.
 * @param ip IPv4 address to resolve, in network byte order.
 * @param now Current DPDK timer cycles.
 * @return Cached Ethernet address when reachable, otherwise NULL.
 */
const uint8_t *arp_resolve(struct rte_mempool *mp, struct rte_ring *out,
                           uint32_t ip, uint64_t now);

/**
 * @brief Learn an IPv4-to-MAC mapping from an ARP packet.
 *
 * @param ip  IPv4 address in network byte order.
 * @param mac Ethernet address to associate with @p ip.
 */
void arp_table_learn(uint32_t ip, const uint8_t *mac);

/**
 * @brief Confirm an IPv4 neighbour from an inbound IPv4 packet.
 *
 * A reachable entry with the same MAC takes a fast path that only refreshes
 * its timestamps. Cache misses, unresolved entries, and MAC changes are
 * learned so reply traffic can use the packet's source Ethernet address.
 *
 * @param ip  IPv4 address in network byte order.
 * @param mac Source Ethernet address from the inbound IPv4 packet.
 */
void arp_table_confirm(uint32_t ip, const uint8_t *mac);

/**
 * @brief Expire stale neighbours and reclaim failed resolution attempts.
 *
 * @param now Current DPDK timer cycles.
 */
void arp_maintain(uint64_t now);

/**
 * @brief Probe a small /24 slice for explicit diagnostic sweeps.
 *
 * Normal data paths must use @ref arp_resolve instead.  This helper is called
 * only when ENABLE_ARP_SWEEP is enabled and is batch-limited by config.h.
 */
void arp_debug_sweep(struct rte_mempool *mp, struct rte_ring *out,
                     uint64_t now);

/**
 * @brief Build an ARP request/reply packet into a fresh mbuf.
 *
 * @param mp      Mempool used to allocate the mbuf.
 * @param opcode  RTE_ARP_OP_REQUEST or RTE_ARP_OP_REPLY.
 * @param dst_mac Target Ethernet address.
 * @param src_ip  Sender IPv4 (network order).
 * @param dst_ip  Target IPv4 (network order).
 * @return Newly allocated mbuf, or NULL on allocation failure.
 */
struct rte_mbuf *arp_build_pkt(struct rte_mempool *mp, uint16_t opcode,
                               const uint8_t *dst_mac, uint32_t src_ip,
                               uint32_t dst_ip);

/**
 * @brief Handle one inbound ARP frame.
 *
 * Replies to requests addressed to the local IP and learns replies into the
 * table. The inbound mbuf is always freed before returning.
 *
 * @param mp   Mempool for any reply packet.
 * @param mbuf Inbound ARP frame (consumed by this call).
 * @param out  Ring on which reply packets are enqueued.
 */
void arp_handle(struct rte_mempool *mp, struct rte_mbuf *mbuf,
                struct rte_ring *out);

#endif /* NETARCH_ARP_H */
