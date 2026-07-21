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
#include <stdint.h>

/**
 * @brief Identity of the local DPDK endpoint.
 */
struct net_context {
        uint16_t port_id;  /**< DPDK ethernet port id. */
        uint32_t local_ip; /**< Local IPv4, network order. */
        uint8_t local_mac[RTE_ETHER_ADDR_LEN]; /**< Local Ethernet address. */
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

#endif /* NETARCH_NET_CONTEXT_H */
