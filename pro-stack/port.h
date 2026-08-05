/**
 * @file port.h
 * @brief Ethernet port initialization.
 */
#ifndef NETARCH_PORT_H
#define NETARCH_PORT_H

#include <rte_mempool.h>
#include <stdint.h>

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
 * @brief Configure and start a port with an equal number of RX and TX queues.
 *
 * When more than one queue is requested, IPv4 TCP/UDP RSS is enabled so each
 * 4-tuple is consistently delivered to one receive queue.  Callers must
 * arrange for exactly one packet worker to poll each queue; configuring extra
 * queues without polling them drops traffic.
 *
 * Terminates the process through rte_exit() on configuration failure or when
 * the NIC does not support the requested queue count or RSS hash functions.
 *
 * @param port_id DPDK port to bring up.
 * @param mp Mempool backing every receive queue.
 * @param queue_count Number of RX queues and TX queues to create.
 */
void port_init_queues(uint16_t port_id, struct rte_mempool *mp,
                      uint16_t queue_count);

#endif /* NETARCH_PORT_H */
