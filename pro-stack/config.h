/**
 * @file config.h
 * @brief Compile-time feature toggles and global constants.
 *
 * All tunables for the demo stack live here so the rest of the code can stay
 * free of magic numbers. Toggle a feature by editing the ENABLE_* switches.
 */
#ifndef NETARCH_CONFIG_H
#define NETARCH_CONFIG_H

#include <stdint.h>

/** Build the transmit path (queue setup + reply packets). */
#define ENABLE_SEND 1
/** Answer ARP requests and learn ARP replies. */
#define ENABLE_ARP 1
/** Answer ICMP echo requests (ping). */
#define ENABLE_ICMP 1
/** Dispatch received UDP datagrams to the userspace socket layer. */
#define ENABLE_UDP_ECHO 1
/** Periodically sweep the subnet with ARP requests. */
#define ENABLE_ARP_SWEEP 1
/** Enable the packet-capture framework so dpdk-pdump can attach. */
#define ENABLE_PDUMP 1
/** Launch the example UDP echo application on a dedicated lcore. */
#define ENABLE_UDP_APP 1
/** Enable the TCP stack ops and launch the example TCP echo application. */
#define ENABLE_TCP_APP 1

/** Number of mbufs in the packet pool. */
#define NUM_MBUFS (4096 - 1)
/** Maximum packets pulled/pushed per burst. */
#define BURST_SIZE 32
/** Depth of the rx/tx software rings. */
#define RING_SIZE 1024
/** Rx/Tx descriptor ring size for the NIC queues. */
#define NB_DESC 1024
/** TSC cycles between periodic timer sweeps (~ tens of seconds). */
#define TIMER_RESOLUTION_CYCLES 120000000000ULL

/**
 * @brief Build an IPv4 address in network byte order from its four octets.
 *
 * The result is laid out so the first octet occupies the least significant
 * byte, which matches how DPDK stores addresses on little-endian hosts.
 */
#define MAKE_IPV4_ADDR(a, b, c, d)                                             \
        ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |        \
         ((uint32_t)(d) << 24))

#endif /* NETARCH_CONFIG_H */
