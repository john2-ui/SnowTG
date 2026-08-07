/**
 * @file port.h
 * @brief Ethernet port initialization.
 */
#ifndef NETARCH_PORT_H
#define NETARCH_PORT_H

#include <rte_mempool.h>
#include <stdbool.h>
#include <stdint.h>

/** Receive topology selected after probing the DPDK ethernet device. */
enum port_rx_mode {
        PORT_RX_MODE_SINGLE_QUEUE,
        PORT_RX_MODE_HARDWARE_RSS,
        PORT_RX_MODE_SOFTWARE_DISPATCH,
};

/**
 * Queue layout chosen by @ref port_init_queues.
 *
 * @c worker_count is the number of software flow buckets. It remains the
 * packet-worker count even when hardware RSS falls back to one RX queue.
 */
struct port_topology {
        enum port_rx_mode rx_mode;
        uint16_t rx_queue_count;
        uint16_t tx_queue_count;
        uint16_t worker_count;
        uint64_t rss_hf;
};

/**
 * @brief Configure and start a DPDK ethernet port with one rx/tx queue.
 *
 * Terminates the process through rte_exit() on any failure.
 *
 * @param port_id DPDK port id to bring up.
 * @param mp      Mempool backing the receive queue.
 */
void port_init(uint16_t port_id, struct rte_mempool *mp);

/**
 * @brief Configure and start a port for @p worker_count packet workers.
 *
 * When the device supports IPv4 TCP RSS, creates one RX/TX queue per worker
 * and enables RSS. Otherwise it creates one RX queue and up to one TX queue
 * per worker; the caller uses the returned topology to software-dispatch RX
 * packets. UDP RSS is enabled when the NIC advertises it.
 *
 * RSS setup failures automatically retry the one-RX-queue topology. Other
 * configuration failures terminate the process through rte_exit().
 *
 * @param port_id DPDK port to bring up.
 * @param mp Mempool backing every receive queue.
 * @param worker_count Number of packet-worker flow buckets.
 * @return The actual RX/TX queue topology.
 */
struct port_topology port_init_queues(uint16_t port_id,
                                      struct rte_mempool *mp,
                                      uint16_t worker_count);
/**
 * Return the configured flow worker for an IPv4 four-tuple, or -1 when flow
 * prediction is unavailable. Arguments use wire byte order.
 *
 * TCP and UDP use the same Toeplitz tuple shape. Other IPv4 protocols may
 * supply zero for both ports to obtain a stable L3 fallback bucket.
 */
int port_flow_queue_for_ipv4(uint8_t protocol, uint32_t remote_ip,
                             uint32_t local_ip, uint16_t remote_port,
                             uint16_t local_port);
/** Return non-zero when @p protocol was distributed by hardware RSS. */
bool port_rx_uses_hardware_rss(uint8_t protocol);
/** Backward-compatible TCP flow-bucket helper. */
int port_rss_queue_for_tcp(uint32_t remote_ip, uint32_t local_ip,
                           uint16_t remote_port, uint16_t local_port);

#ifdef PORT_TESTING
/** Test seam: configure the software hash state without a NIC. */
int port_test_configure_software_rss(uint16_t worker_count);
/** Test seam: emulate a hardware RSS topology without a NIC. */
int port_test_configure_hardware_rss(uint16_t worker_count, uint64_t rss_hf);
/** Test seam: clear the software hash state. */
void port_test_reset(void);
#endif

#endif /* NETARCH_PORT_H */
