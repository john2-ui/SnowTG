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
/** Enable rate-limited /24 ARP sweep for controlled diagnostics only. */
#define ENABLE_ARP_SWEEP 0
/**
 * ARP sweep is a diagnostic-only facility.  Normal operation uses the
 * demand-driven resolver below and must not probe every host in a subnet.
 */
#define ARP_SWEEP_INTERVAL_MS (60U * 1000U)
#define ARP_SWEEP_BATCH 8U

/** Maximum number of IPv4 neighbours retained by the ARP cache. */
#define ARP_CACHE_CAPACITY 256U
/** Time a learned neighbour remains usable without being refreshed. */
#define ARP_REACHABLE_TTL_MS (5U * 60U * 1000U)
/** Minimum interval between ARP requests for one unresolved neighbour. */
#define ARP_PROBE_INTERVAL_MS 1000U
/** Number of ARP requests made before a neighbour enters FAILED state. */
#define ARP_PROBE_MAX_RETRIES 3U
/** Backoff before a failed neighbour may be resolved again. */
#define ARP_FAILED_TTL_MS (30U * 1000U)
/** Frequency at which the packet worker expires ARP cache entries. */
#define ARP_MAINTENANCE_INTERVAL_MS 1000U
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
#define TCP_CLIENT_PEER_IP MAKE_IPV4_ADDR(192, 168, 21, 106)

/** Number of mbufs in the packet pool. */
#define NUM_MBUFS (16384 - 1)
/** Maximum packets pulled/pushed per burst. */
#define BURST_SIZE 32
/** Maximum dirty sockets flushed by one owner-worker turn. */
#define TX_DIRTY_BUDGET 64
/** Depth of the rx/tx software rings. */
#define RING_SIZE 1024
/** Rx/Tx descriptor ring size for the NIC queues. */
#define NB_DESC 1024
/**
 * Maximum number of datagrams retained by one owner-local UDP socket.
 *
 * This is a logical queue limit only.  Local UDP does not allocate a
 * per-socket DPDK ring; queue nodes are acquired lazily from the owner's
 * UDP memory domain.
 */
#define UDP_RX_QUEUE_LIMIT 64U
/** Per-owner capacity of lazy owner-local UDP RX queue nodes. */
#define UDP_MEMORY_RX_NODES 4095U
/** Minimum IPv4 MTU required by RFC 791. */
#define IPV4_MIN_MTU 68U
/** Maximum ordinary UDP datagrams emitted by one large sendto call. */
#define UDP_SENDTO_MAX_DATAGRAMS 8U
/** Maximum simultaneous fragmented IPv4 datagrams retained for reassembly. */
#define IPV4_REASSEMBLY_MAX_ENTRIES 1024U
/** Lifetime of an incomplete IPv4 datagram. */
#define IPV4_REASSEMBLY_TIMEOUT_MS 30000U
/** Period between explicit reassembly-table expiry sweeps. */
#define IPV4_REASSEMBLY_SWEEP_MS 1000U

/**
 * How often timer-owning lcores call rte_timer_manage(), in milliseconds.
 * The main lcore manages ARP infrastructure timers; the packet worker manages
 * TCP TCB timers so callbacks obey socket ownership.
 * Actual cycle threshold is computed at runtime:
 *   rte_get_timer_hz() * TIMER_MANAGE_INTERVAL_MS / 1000
 */
#define TIMER_MANAGE_INTERVAL_MS 10

/**
 * Maximum application bytes retained per TCP stream before backpressure.
 * Storage is allocated lazily in owner-local chunks, not preallocated per TCB.
 */
#define TCP_SNDBUF_SIZE (64 * 1024)
/** Application-visible TCP send buffer high watermark. */
#define TCP_SNDBUF_APP_HIWAT TCP_SNDBUF_SIZE
/** Bytes carried by one owner-local TCP payload block. */
#define TCP_MEMORY_CHUNK_SIZE 2048U
/** Per-owner capacity of TCP data chunks retained until ACK. */
#define TCP_MEMORY_TX_CHUNKS 8191U
/** Per-owner capacity of queued contiguous receive blobs. */
#define TCP_MEMORY_RX_BLOBS 4095U
/** Per-owner capacity of out-of-order segment descriptors. */
#define TCP_MEMORY_OFO_SEGS 4095U
/** Per-owner capacity of queued SYN/ACK/FIN descriptor objects. */
#define TCP_MEMORY_FRAGMENTS 4095U
/** Per-owner capacity of lazily allocated sender SACK scoreboard ranges. */
#define TCP_MEMORY_SACK_RANGES 8191U
/** Shared per-owner payload backing blocks for RX, OFO, and TX chunks. */
#define TCP_MEMORY_PAYLOAD_BLOCKS 8191U
/** Pause traffic-gen admissions below this per-pool availability threshold. */
#define TCP_MEMORY_LOW_WATER 64U
/** Resume admissions only after all pools exceed this hysteresis threshold. */
#define TCP_MEMORY_HIGH_WATER 128U
/** Default MSS used when slicing sndbuf for TX (no option negotiation yet). */
#define TCP_DEFAULT_MSS 1460

/** Congestion-control algorithm identifiers used by TCP_CC_DEFAULT_ALGO. */
#define TCP_CC_ALGO_NEWRENO 1U
#define TCP_CC_ALGO_CUBIC 2U
/** Congestion-control algorithm selected for every newly initialized TCB. */
#ifndef TCP_CC_DEFAULT_ALGO
#define TCP_CC_DEFAULT_ALGO TCP_CC_ALGO_CUBIC
#endif
#if TCP_CC_DEFAULT_ALGO != TCP_CC_ALGO_NEWRENO &&                           \
    TCP_CC_DEFAULT_ALGO != TCP_CC_ALGO_CUBIC
#error "TCP_CC_DEFAULT_ALGO must select NewReno or CUBIC"
#endif
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
/** Inclusive ephemeral local-port range shared by TCP and UDP. */
#define EPHEMERAL_PORT_MIN 49152
#define EPHEMERAL_PORT_MAX 65535
/** Compatibility aliases for implicit bind in tcp_connect. */
#define TCP_EPHEMERAL_PORT_MIN EPHEMERAL_PORT_MIN
#define TCP_EPHEMERAL_PORT_MAX EPHEMERAL_PORT_MAX
/** UDP's implicit-bind range used by owner-local datagram flows. */
#define UDP_EPHEMERAL_PORT_MIN EPHEMERAL_PORT_MIN
#define UDP_EPHEMERAL_PORT_MAX EPHEMERAL_PORT_MAX
/** Maximum RB-tree/DLL-indexed out-of-order segments buffered per TCB. */
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
