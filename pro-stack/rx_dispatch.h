/**
 * @file rx_dispatch.h
 * @brief Main-lcore IPv4 flow classification for software RSS fallback.
 */
#ifndef NETARCH_RX_DISPATCH_H
#define NETARCH_RX_DISPATCH_H

#include <rte_mbuf.h>

#include <stdbool.h>
#include <stdint.h>

enum rx_dispatch_action {
        RX_DISPATCH_DELIVER,
        RX_DISPATCH_FANOUT,
};

/**
 * Result of classifying one NIC RX mbuf.
 *
 * @c worker_index is valid for both actions. For @c RX_DISPATCH_FANOUT it
 * identifies the worker receiving the original mbuf; callers clone it to
 * every other worker.
 */
struct rx_dispatch_result {
        enum rx_dispatch_action action;
        uint16_t worker_index;
        bool owner_hit;
        bool software_hash;
        bool parse_fallback;
};

/**
 * Install the packet-worker lcore mapping before any owner can publish an
 * endpoint. Called by the I/O lcore during startup.
 */
int rx_dispatch_configure_workers(const unsigned int *lcores,
                                  uint16_t worker_count);
/** Clear published endpoints after all owner workers have stopped. */
void rx_dispatch_reset(void);

/**
 * Publish or remove endpoint ownership as socket registries change.
 *
 * These are no-ops when no dispatcher is configured, preserving single-worker
 * applications that do not use this module.
 */
int rx_dispatch_register_endpoint(uint8_t protocol, uint32_t local_ip,
                                  uint16_t local_port,
                                  unsigned int owner_lcore);
void rx_dispatch_unregister_endpoint(uint8_t protocol, uint32_t local_ip,
                                     uint16_t local_port,
                                     unsigned int owner_lcore);
/** Return non-zero when an endpoint owner has already reserved this address. */
bool rx_dispatch_endpoint_is_registered(uint8_t protocol, uint32_t local_ip,
                                        uint16_t local_port);
int rx_dispatch_register_tcp_connection(uint32_t remote_ip,
                                        uint32_t local_ip,
                                        uint16_t remote_port,
                                        uint16_t local_port,
                                        unsigned int owner_lcore);
void rx_dispatch_unregister_tcp_connection(uint32_t remote_ip,
                                           uint32_t local_ip,
                                           uint16_t remote_port,
                                           uint16_t local_port,
                                           unsigned int owner_lcore);

/**
 * Select the worker that must receive @p mbuf received from @p rx_queue.
 *
 * ARP returns @c RX_DISPATCH_FANOUT. Short, fragmented, or unsupported
 * packets return worker zero so the normal stack validation owns the drop.
 */
void rx_dispatch_classify(const struct rte_mbuf *mbuf, uint16_t rx_queue,
                          struct rx_dispatch_result *out);

#endif /* NETARCH_RX_DISPATCH_H */
