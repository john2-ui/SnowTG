#ifndef NETARCH_STACK_RUNTIME_H
#define NETARCH_STACK_RUNTIME_H

#include "owner_timer.h"

/**
 * @file stack_runtime.h
 * @brief Owner-worker loop integration point for upper-layer reactors.
 */

#include <stdint.h>

struct rte_mempool;
struct rte_mbuf;
struct inout_ring;
struct ipv4_reassembly;

typedef void (*stack_runtime_reactor_fn)(void *ctx, unsigned int budget);

/** Owner-local counters accumulated between traffic-generator reports. */
struct stack_runtime_metrics {
        uint64_t worker_turns;       /**< Completed worker-loop iterations. */
        uint64_t rx_packets;         /**< Frames delivered to the local protocol stack. */
        uint64_t tx_flush_calls;     /**< Dirty-socket transport flush calls. */
        /** Dirty queue entries dequeued; retained under the old log name. */
        uint64_t socket_scans;
        uint64_t dirty_tx_enqueues;
        uint64_t dirty_tx_dedup_hits;
        uint64_t dirty_tx_dequeues;
        uint64_t dirty_tx_requeues;
        uint64_t dirty_tx_arp_waits;
        uint64_t dirty_tx_arp_wakeups;
        uint64_t dirty_tx_budget_exhausted;
        uint64_t udp_tx_queue_drops; /**< Owner-local UDP queue drops. */
        uint64_t turn_cycles;        /**< End-to-end worker-loop time. */
        uint64_t rx_cycles;          /**< Ring/NIC receive plus ingress time. */
        uint64_t maintenance_cycles; /**< Timer and ARP maintenance time. */
        uint64_t reactor_cycles;     /**< Upper-layer reactor callback time. */
        uint64_t tx_flush_cycles;    /**< Dirty queue drain time. */
        uint64_t tx_packets;         /**< Packets accepted by the NIC. */
        uint64_t tx_bursts;
        uint64_t tx_nic_drops;
        uint64_t nic_tx_cycles;      /**< Sampled NIC calls only. */
        uint64_t nic_tx_sampled_packets;
        uint64_t nic_tx_sampled_bursts;
        uint64_t nic_rx_packets;
        uint64_t rx_burst_calls;
        uint64_t rx_empty_bursts;
        uint64_t rx_full_bursts;
        uint64_t rx_handoffs;
        uint64_t rx_handoff_drops;
        uint32_t
            in_ring_high_water; /**< Largest observed NIC-to-worker depth. */
        uint32_t
            out_ring_high_water; /**< Largest observed worker-to-NIC depth. */
        uint32_t dirty_tx_high_water; /**< Largest dirty queue depth. */
        uint32_t dirty_tx_depth;      /**< Dirty queue depth at snapshot time. */
        /** Owner-local TCP out-of-order queue gauges and interval counters. */
        uint64_t ofo_segments_current;
        uint64_t ofo_segments_peak;
        uint64_t ofo_bytes_current;
        uint64_t ofo_bytes_peak;
        uint64_t ofo_accepted_segments;
        uint64_t ofo_accepted_bytes;
        uint64_t ofo_released_segments;
        uint64_t ofo_released_bytes;
        uint64_t ofo_reorder_distance_max;
        uint64_t ofo_drop_rcvbuf;
        uint64_t ofo_drop_seg_limit;
        uint64_t ofo_drop_byte_limit;
        uint64_t ofo_drop_owner_limit;
        uint64_t ofo_drop_alloc;
        uint64_t ofo_drop_pressure;
        uint64_t ofo_pressure_transitions;
        uint64_t ofo_pressure_active;
};

/**
 * Per-worker state supplied when launching the packet-worker loop.
 */
struct stack_runtime_worker {
        unsigned int lcore_id;
        uint16_t queue_id;
        uint16_t port_id;
        uint16_t tx_queue_id;
        bool direct_tx_enabled;
        bool direct_rx_enabled;
        uint16_t rx_queue_id;
        /** Only queue zero owns fragmented/control-plane packet assembly. */
        struct ipv4_reassembly *reassembly;
        uint32_t tx_sample_every;
        uint32_t tx_until_sample;
        struct rte_mempool *mp;
        struct inout_ring *ring;
        stack_runtime_reactor_fn reactor;
        void *reactor_ctx;
        /** Optional owner-local final snapshot, after the last complete turn. */
        void (*on_exit)(void *reactor_ctx);
        struct stack_runtime_metrics metrics;
        struct owner_timer_engine timer_engine;
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
                              uint32_t timer_capacity, struct rte_mempool *mp,
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

/** Owner-only, bounded TX. UINT_MAX drains to empty after producers stop. */
void stack_runtime_tx_drain(struct stack_runtime_worker *worker,
                            unsigned int burst_budget, bool sample);

/** Consume one direct-RX/forwarded frame on its current owner lcore. */
void stack_runtime_rx_process(struct stack_runtime_worker *worker,
                              struct rte_mbuf *mbuf, uint64_t now_cycles);

/** DPDK lcore entry point: packet ingress, timers, reactor, and TX flush. */
int stack_runtime_worker_entry(void *arg);

#endif /* NETARCH_STACK_RUNTIME_H */
