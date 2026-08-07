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
};

/**
 * @brief Create the SPSC ring pair assigned to one packet-worker lcore.
 */
int ring_init_owner(unsigned int lcore_id);
/** Return a specific worker's ring pair, or NULL when it is not initialized. */
struct inout_ring *ring_for_lcore(unsigned int lcore_id);
/** Return the current lcore's ring pair. */
struct inout_ring *ring_instance(void);
/** Release every initialized ring pair after all workers have stopped. */
void ring_fini(void);

#endif /* NETARCH_RING_H */
