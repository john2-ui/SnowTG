/**
 * @file ring.h
 * @brief Rx/Tx software rings shared between the I/O loop and the worker.
 *
 * The receive loop pushes inbound packets onto @c in; the worker pops them,
 * builds replies and pushes those onto @c out; the I/O loop then transmits
 * whatever is on @c out.
 */
#ifndef NETARCH_RING_H
#define NETARCH_RING_H

#include <rte_ring.h>

/**
 * @brief A pair of single-producer/consumer style rings.
 */
struct inout_ring {
        struct rte_ring *in;  /**< NIC -> worker queue. */
        struct rte_ring *out; /**< worker -> NIC queue. */
        /**
         * TCP application -> worker notifications that receive bytes were
         * consumed. Multiple app lcores may produce; the packet worker is the
         * sole consumer. Entries are @c struct nsock pointers.
         */
        struct rte_ring *tcp_rx_events;
};

/**
 * @brief Get the process-wide ring pair, creating it on first use.
 *
 * @return Pointer to the initialized ring pair. Terminates on failure.
 */
struct inout_ring *ring_instance(void);

#endif /* NETARCH_RING_H */
