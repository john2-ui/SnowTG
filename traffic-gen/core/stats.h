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

#include <stdbool.h>
#include <stdint.h>

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
        uint64_t bytes_tx;
        uint64_t bytes_rx;
        uint64_t http_rps_total;
        uint32_t concurrency;
        uint64_t last_report_cycles;
};

/** @brief Clears all counters and report-timing state. */
void tg_stats_init(struct tg_stats *stats);

/**
 * @brief Records a successfully admitted flow.
 * @param stats Owner-local counters to update.
 */
void tg_stats_on_admitted(struct tg_stats *stats);

/**
 * @brief Records a synchronous failure before a flow becomes active.
 * @param stats Owner-local counters to update.
 *
 * Such failures count as both started and done, and are classified as connect
 * failures without changing @ref tg_stats::concurrency.
 */
void tg_stats_on_start_failure(struct tg_stats *stats);

/**
 * @brief Records terminal flow result before the flow is returned to its pool.
 * @param stats Owner-local counters to update.
 * @param flow Completed flow whose transaction byte counts are still intact.
 * @param result Terminal result selected by the transport layer.
 */
void tg_stats_on_flow_finished(struct tg_stats *stats,
                               const struct tg_flow *flow,
                               enum tg_flow_result result);

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
