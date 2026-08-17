#ifndef NETARCH_IPV4_REASSEMBLY_H
#define NETARCH_IPV4_REASSEMBLY_H

/**
 * @file ipv4_reassembly.h
 * @brief Main-lcore IPv4 fragment reassembly before owner dispatch.
 */

#include <rte_ip_frag.h>
#include <stdint.h>

struct rte_mbuf;

struct ipv4_reassembly {
        struct rte_ip_frag_tbl *table;
        struct rte_ip_frag_death_row death_row;
        uint64_t sweep_interval_cycles;
        uint64_t last_sweep_cycles;
};

/** Initialize one main-lcore reassembly context. */
int ipv4_reassembly_init(struct ipv4_reassembly *ctx);
/**
 * Process one Ethernet frame. Unfragmented frames pass through unchanged.
 * Fragmented frames are consumed; a non-NULL result is returned only after
 * complete reassembly.
 */
struct rte_mbuf *ipv4_reassembly_process(struct ipv4_reassembly *ctx,
                                         struct rte_mbuf *mbuf,
                                         uint64_t now_cycles);
/** Expire incomplete datagrams at the configured maintenance cadence. */
void ipv4_reassembly_maintain(struct ipv4_reassembly *ctx, uint64_t now_cycles);
/** Release retained fragments and destroy the table. */
void ipv4_reassembly_fini(struct ipv4_reassembly *ctx);

#endif /* NETARCH_IPV4_REASSEMBLY_H */
