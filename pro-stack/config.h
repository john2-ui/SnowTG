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

/**
 * @brief Build an IPv4 address in network byte order from its four octets.
 *
 * The result is laid out so the first octet occupies the least significant
 * byte, which matches how DPDK stores addresses on little-endian hosts.
 */
#define MAKE_IPV4_ADDR(a, b, c, d)                                             \
        ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |        \
         ((uint32_t)(d) << 24))

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
#define ENABLE_UDP_APP 0
/** Enable UDP debug logging. */
#define ENABLE_UDP_DEBUG 0
/** Enable the TCP stack ops (required for either TCP app below). */
#define ENABLE_TCP_APP 1
/** Launch the TCP echo server (listen/accept/recv/send/close). */
#define ENABLE_TCP_SERVER 1
/**
 * Launch the TCP client (connect/send/recv/close). Off by default so a single
 * host can run the server; flip on (and usually turn ENABLE_TCP_SERVER off)
 * when driving active-open against a peer.
 */
#define ENABLE_TCP_CLIENT 0

/** Well-known port used by the TCP echo server / client peer. */
#define TCP_APP_PORT 8888
/** Peer IPv4 for the TCP client (network order via MAKE_IPV4_ADDR). */
#define TCP_CLIENT_PEER_IP MAKE_IPV4_ADDR(192, 168, 21, 105)

/** Number of mbufs in the packet pool. */
#define NUM_MBUFS (4096 - 1)
/** Maximum packets pulled/pushed per burst. */
#define BURST_SIZE 32
/** Depth of the rx/tx software rings. */
#define RING_SIZE 1024
/** Rx/Tx descriptor ring size for the NIC queues. */
#define NB_DESC 1024

/**
 * How often timer-owning lcores call rte_timer_manage(), in milliseconds.
 * The main lcore manages ARP infrastructure timers; the packet worker manages
 * TCP TCB timers so callbacks obey socket ownership.
 * Actual cycle threshold is computed at runtime:
 *   rte_get_timer_hz() * TIMER_MANAGE_INTERVAL_MS / 1000
 */
#define TIMER_MANAGE_INTERVAL_MS 10

/** TCP send buffer (sliding window) size in bytes. */
#define TCP_SNDBUF_SIZE (64 * 1024)
/** Application-visible TCP send buffer high watermark. */
#define TCP_SNDBUF_APP_HIWAT TCP_SNDBUF_SIZE
/** Default MSS used when slicing sndbuf for TX (no option negotiation yet). */
#define TCP_DEFAULT_MSS 1460
/** RFC 6298 initial data/FIN RTO before a valid RTT sample (ms). */
#define TCP_RTO_INITIAL_MS 1000
/** RFC 6298 lower bound for the calculated data/FIN RTO (ms). */
#define TCP_RTO_MIN_MS 1000
/** Safety cap for exponential data/FIN RTO backoff (ms). */
#define TCP_RTO_MAX_MS 60000
/** First SYN retransmit timeout (ms). */
#define TCP_SYN_RTO_MS 1000
/** Give up after this many data RTOs. */
#define TCP_DATA_MAX_RETRIES 5
/** Give up after this many SYN retransmits (not counting the first SYN). */
#define TCP_SYN_MAX_RETRIES 5
/** Aliases for handshake control-segment RTO (SYN / SYN+ACK share SYN_*). */
#define TCP_CTRL_RTO_MS TCP_SYN_RTO_MS
#define TCP_CTRL_MAX_RETRIES TCP_SYN_MAX_RETRIES
/** Inclusive ephemeral local-port range for implicit bind in tcp_connect. */
#define TCP_EPHEMERAL_PORT_MIN 49152
#define TCP_EPHEMERAL_PORT_MAX 65535
/** Max out-of-order segments buffered per TCB (DLL today; see ofo rb-tree
 * TODO). */
#define TCP_OFO_MAX_SEGS 32

#define TCP_MSL_MS 1000
#define TCP_2MSL_MS (2 * TCP_MSL_MS)

/**
 * Default receive buffer size (bytes).  Values above UINT16_MAX require the
 * RFC 7323 Window Scale option, negotiated by TCP during the handshake.
 */
#define TCP_RCVBUF_SIZE (256U * 1024U)
/** Maximum OFO payload bytes retained by one TCP control block. */
#define TCP_OFO_MAX_BYTES TCP_RCVBUF_SIZE
/** Process-wide cap for copied OFO payload bytes across all TCP streams. */
#define TCP_OFO_GLOBAL_MAX_BYTES (4U * 1024U * 1024U)

#endif /* NETARCH_CONFIG_H */
