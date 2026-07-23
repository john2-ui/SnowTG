/**
 * @file net_context.h
 * @brief Local network identity shared by every protocol module.
 *
 * The original code kept the local IP/MAC and the working port id in scattered
 * globals. They are gathered here into a single context so the protocol helpers
 * can read a well-defined source identity instead of file-local statics.
 */
#ifndef NETARCH_NET_CONTEXT_H
#define NETARCH_NET_CONTEXT_H

#include <rte_ether.h>
#include <rte_mempool.h>
#include <stdint.h>

/**
 * @brief Identity of the local DPDK endpoint.
 */
struct net_context {
        uint16_t port_id;  /**< DPDK ethernet port id. */
        uint32_t local_ip; /**< Local IPv4, network order. */
        uint8_t local_mac[RTE_ETHER_ADDR_LEN]; /**< Local Ethernet address. */
        struct rte_mempool *mp; /**< Process-wide mbuf pool for TX/RX. */
};

/** Process-wide network identity, populated by net_context_init(). */
extern struct net_context g_net;

/**
 * @brief Initialize the global context and read the port MAC address.
 *
 * @param port_id  DPDK port to operate on.
 * @param local_ip Local IPv4 address in network byte order.
 */
void net_context_init(uint16_t port_id, uint32_t local_ip);

/**
 * @brief Attach the global mbuf pool after EAL/mempool creation.
 *
 * Socket API and protocol helpers read this through @c g_net.mp instead of
 * taking a mempool argument on every send/recv call.
 */
void net_context_set_mempool(struct rte_mempool *mp);

#endif /* NETARCH_NET_CONTEXT_H */
