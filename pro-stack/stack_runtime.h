#ifndef NETARCH_STACK_RUNTIME_H
#define NETARCH_STACK_RUNTIME_H

/**
 * @file stack_runtime.h
 * @brief Owner-worker loop integration point for upper-layer reactors.
 */

#include <stdint.h>

struct rte_mempool;
struct inout_ring;

typedef void (*stack_runtime_reactor_fn)(void *ctx, unsigned int budget);

/** Owner-local counters accumulated between traffic-generator reports. */
struct stack_runtime_metrics {
        uint64_t worker_turns;       /**< Completed worker-loop iterations. */
        uint64_t rx_packets;         /**< Packets dispatched from ring->in. */
        uint64_t tx_flush_calls;     /**< Per-socket transport flush calls. */
        uint64_t socket_scans;       /**< Nodes visited in the local socket list. */
        uint64_t turn_cycles;        /**< End-to-end worker-loop time. */
        uint64_t rx_cycles;          /**< ring->in dequeue plus ingress time. */
        uint64_t maintenance_cycles; /**< Timer and ARP maintenance time. */
        uint64_t reactor_cycles;     /**< Upper-layer reactor callback time. */
        uint64_t tx_flush_cycles;    /**< Full socket-list TX scan time. */
        uint32_t
            in_ring_high_water; /**< Largest observed NIC-to-worker depth. */
        uint32_t
            out_ring_high_water; /**< Largest observed worker-to-NIC depth. */
};

/**
 * Per-worker state supplied when launching the packet-worker loop.
 */
struct stack_runtime_worker {
        unsigned int lcore_id;
        uint16_t queue_id;
        struct rte_mempool *mp;
        struct inout_ring *ring;
        stack_runtime_reactor_fn reactor;
        void *reactor_ctx;
        struct stack_runtime_metrics metrics;
        uint64_t last_timer_tsc;
        uint64_t last_arp_maintenance_tsc;
        uint64_t last_arp_sweep_tsc;
};

/**
 * Configure one owner worker before it is launched. The caller retains
 * @p worker storage until rte_eal_wait_lcore() has returned.
 */
int stack_runtime_worker_init(struct stack_runtime_worker *worker,
                              unsigned int lcore_id, uint16_t queue_id,
                              struct rte_mempool *mp,
                              struct inout_ring *ring,
                              stack_runtime_reactor_fn reactor,
                              void *reactor_ctx);
/** Return the NIC queue assigned to an initialized packet-worker lcore. */
int stack_runtime_queue_for_lcore(unsigned int lcore_id, uint16_t *queue_out);
/** Request orderly termination of all stack runtime worker loops. */
void stack_runtime_request_stop(void);
/** Return non-zero once runtime termination has been requested. */
int stack_runtime_stop_requested(void);
/**
 * Return and clear owner-local worker metrics accumulated since the previous
 * call. This is valid only on the packet worker lcore.
 */
void stack_runtime_metrics_take(struct stack_runtime_metrics *out);

/** DPDK lcore entry point: packet ingress, timers, reactor, and TX flush. */
int stack_runtime_worker_entry(void *arg);

#endif /* NETARCH_STACK_RUNTIME_H */
