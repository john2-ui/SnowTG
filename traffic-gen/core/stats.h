#ifndef TRAFFIC_GEN_STATS_H
#define TRAFFIC_GEN_STATS_H

/**
 * @file stats.h
 * @brief Per-owner-lcore transaction counters and report cadence helpers.
 *
 * Counters are intentionally non-atomic because exactly one owner worker
 * updates a shard's state.  A future control plane can sample these counters
 * at low frequency without placing atomic operations in the flow hot path.
 */

#include "flow.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define TG_STATS_CHANNEL_CAP 16

enum tg_stats_snapshot_phase {
        TG_STATS_PHASE_PERIODIC = 0,
        TG_STATS_PHASE_FINAL = 1,
};

/**
 * @brief Fixed-format owner snapshot consumed by the CSV writer.
 *
 * Transaction and latency fields are cumulative. Runtime, reactor, scheduler
 * and drop fields describe the interval that produced the snapshot, except in
 * a final snapshot where they cover the complete run.
 */
struct tg_stats_snapshot {
        /** Monotonic timer timestamp converted to microseconds in CSV. */
        uint64_t timestamp_cycles;
        /** Per-worker snapshot sequence; aggregate keeps the maximum. */
        uint64_t sequence;
        /** Worker index; UINT64_MAX identifies the aggregate row. */
        uint64_t worker_index;
        /** DPDK lcore ID; UINT64_MAX identifies the aggregate row. */
        uint64_t lcore_id;
        /** TG_STATS_PHASE_PERIODIC or TG_STATS_PHASE_FINAL. */
        uint64_t phase;

        /** Cumulative transaction counters. */
        uint64_t txns_started;
        uint64_t txns_done;
        uint64_t txns_success;
        uint64_t txns_fail;
        uint64_t fail_connect;
        uint64_t fail_io;
        uint64_t fail_proto;
        uint64_t fail_resource;
        /** Starts deferred because local resources were unavailable. */
        uint64_t starts_deferred_resource;
        uint64_t bytes_tx;
        uint64_t bytes_rx;
        /** Cumulative HTTP successes; this is not a rate despite the name. */
        uint64_t http_rps_total;
        /** Current admitted flows that have not completed. */
        uint64_t concurrency;
        /** Current live socket count. */
        uint64_t live_sockets;
        /** Physical TCP connections created for this shard. */
        uint64_t connections_created;
        /** Logical transactions assigned to an existing TCP connection. */
        uint64_t connections_reused;

        /**
         * Connect latency: start to CONNECTED.
         * Sums/maxima are in timer cycles; CSV avg/max values are in us.
         */
        uint64_t connect_samples;
        uint64_t connect_cycles;
        uint64_t connect_max_cycles;
        /** First-response latency: start to first received byte. */
        uint64_t first_rx_samples;
        uint64_t first_rx_cycles;
        uint64_t first_rx_max_cycles;
        /** Completion latency: start to the terminal flow callback. */
        uint64_t complete_samples;
        uint64_t complete_cycles;
        uint64_t complete_max_cycles;

        /** Resource-pressure state and memory-pool availability metrics. */
        uint64_t memory_paused;
        uint64_t memory_pauses;
        uint64_t tx_available;
        uint64_t tx_peak;
        uint64_t payload_available;
        uint64_t payload_peak;
        uint64_t tx_alloc_fail;

        /**
         * Worker/runtime metrics. Periodic snapshots report the interval;
         * final snapshots report the complete run.
         */
        uint64_t worker_turns;
        uint64_t rx_packets;
        uint64_t socket_scans;
        uint64_t tx_flush_calls;
        /** Dirty-TX queue activity and backlog metrics. */
        uint64_t dirty_tx_enqueues;
        uint64_t dirty_tx_dedup_hits;
        uint64_t dirty_tx_requeues;
        uint64_t dirty_tx_arp_waits;
        uint64_t dirty_tx_arp_wakeups;
        uint64_t dirty_tx_depth;
        uint64_t dirty_tx_high_water;
        uint64_t dirty_tx_budget_exhausted;
        uint64_t turn_cycles;
        uint64_t rx_cycles;
        uint64_t maintenance_cycles;
        uint64_t reactor_cycles;
        uint64_t tx_flush_cycles;

        /** Reactor, scheduler, socket-release, and ring high-water metrics. */
        uint64_t reactor_turns;
        uint64_t reactor_events;
        uint64_t reactor_burst_high_water;
        uint64_t scheduler_starts;
        uint64_t tokens;
        uint64_t socket_releases;
        uint64_t ring_hwm_in;
        uint64_t ring_hwm_out;
        /** Datapath drops and RX dispatch fallback counters. */
        uint64_t rx_ring_drops;
        uint64_t tx_nic_drops;
        uint64_t rx_owner_hits;
        uint64_t rx_software_hashes;
        uint64_t rx_parse_fallbacks;
        /** Snapshots discarded because the worker-to-main channel was full. */
        uint64_t stats_queue_drops;
};

/**
 * @brief Single-producer/single-consumer snapshot channel.
 *
 * The owner worker publishes and the main lcore consumes. A full channel
 * drops only a statistics record; it never stalls packet processing.
 */
struct tg_stats_channel {
        struct tg_stats_snapshot records[TG_STATS_CHANNEL_CAP];
        _Atomic uint64_t write_seq;
        _Atomic uint64_t read_seq;
        _Atomic uint64_t dropped;
};

/**
 * @brief Cumulative transaction, failure, byte, and reporting state.
 *
 * @p concurrency tracks successfully admitted flows that have not yet
 * notified completion.  @p http_rps_total is a cumulative HTTP-success count;
 * it is named for eventual windowed RPS reporting but is not a rate itself.
 */
struct tg_stats {
        uint64_t txns_started;
        uint64_t txns_done;
        uint64_t txns_success;
        uint64_t txns_fail;
        uint64_t fail_connect;
        uint64_t fail_io;
        uint64_t fail_proto;
        uint64_t fail_resource;
        uint64_t starts_deferred_resource;
        uint64_t bytes_tx;
        uint64_t bytes_rx;
        uint64_t http_rps_total;
        /** Owner-local phase latency sums/maxima, stored in timer cycles. */
        uint64_t connect_samples;
        uint64_t connect_cycles; /**< Start to CONNECTED cumulative time. */
        uint64_t connect_max_cycles;
        uint64_t first_rx_samples;
        uint64_t first_rx_cycles; /**< Start to first response byte sum. */
        uint64_t first_rx_max_cycles;
        uint64_t complete_samples;
        uint64_t complete_cycles; /**< Start to terminal flow callback sum. */
        uint64_t complete_max_cycles;
        uint32_t concurrency;
        uint64_t connections_created;
        uint64_t connections_reused;
        uint64_t last_report_cycles;
};

/** @brief Clears all counters and report-timing state. */
void tg_stats_init(struct tg_stats *stats);

/**
 * @brief Records a successfully admitted flow.
 * @param stats Owner-local counters to update.
 */
void tg_stats_on_admitted(struct tg_stats *stats);

/** Records creation of one physical TCP connection. */
void tg_stats_on_connection_created(struct tg_stats *stats);
/** Records assignment of one transaction to an existing connection. */
void tg_stats_on_connection_reused(struct tg_stats *stats);

/**
 * @brief Records a synchronous failure before a flow becomes active.
 * @param stats Owner-local counters to update.
 *
 * Such failures count as both started and done, and are classified as connect
 * failures without changing @ref tg_stats::concurrency.
 */
void tg_stats_on_start_failure(struct tg_stats *stats);
/** Records a local resource refusal without classifying it as remote failure.
 */
void tg_stats_on_resource_deferred(struct tg_stats *stats);

/**
 * @brief Records one logical transaction result before flow reuse or teardown.
 * @param stats Owner-local counters to update.
 * @param flow Flow whose completed transaction byte counts are still intact.
 * @param result Terminal result selected by the transport layer.
 */
void tg_stats_on_flow_finished(struct tg_stats *stats,
                               const struct tg_flow *flow,
                               enum tg_flow_result result);

/** Fill the transaction and latency portion of an owner snapshot. */
void tg_stats_snapshot_from_stats(struct tg_stats_snapshot *snapshot,
                                  const struct tg_stats *stats,
                                  uint64_t timestamp_cycles, uint64_t sequence,
                                  uint64_t worker_index, uint64_t lcore_id,
                                  enum tg_stats_snapshot_phase phase);

/** Aggregate worker snapshots, summing counters and taking relevant maxima. */
void tg_stats_snapshot_add(struct tg_stats_snapshot *aggregate,
                           const struct tg_stats_snapshot *sample);

/** Aggregate only runtime/interval fields into a cumulative runtime snapshot.
 */
void tg_stats_snapshot_add_runtime(struct tg_stats_snapshot *total,
                                   const struct tg_stats_snapshot *sample);

/** Replace runtime/interval fields in @p snapshot with @p total. */
void tg_stats_snapshot_copy_runtime(struct tg_stats_snapshot *snapshot,
                                    const struct tg_stats_snapshot *total);

/** Initialize an owner-to-main snapshot channel. */
void tg_stats_channel_init(struct tg_stats_channel *channel);

/** Publish one snapshot without blocking the owner worker. */
bool tg_stats_channel_publish(struct tg_stats_channel *channel,
                              const struct tg_stats_snapshot *snapshot);

/** Consume one snapshot on the main lcore. */
bool tg_stats_channel_consume(struct tg_stats_channel *channel,
                              struct tg_stats_snapshot *snapshot);

/** Return and clear records dropped because the channel was full. */
uint64_t tg_stats_channel_take_dropped(struct tg_stats_channel *channel);

/**
 * @brief Tests and advances the periodic report timestamp.
 * @param stats Owner-local counters containing the previous report timestamp.
 * @param now_cycles Current cycle-clock timestamp.
 * @param cycles_per_second Cycle-clock frequency.
 * @param report_interval_sec Requested reporting interval in seconds.
 * @return @c true once an interval has elapsed; otherwise @c false.
 */
bool tg_stats_report_due(struct tg_stats *stats, uint64_t now_cycles,
                         uint64_t cycles_per_second,
                         uint32_t report_interval_sec);

#endif /* TRAFFIC_GEN_STATS_H */
