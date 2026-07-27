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

#endif /* NETARCH_PORT_H */
