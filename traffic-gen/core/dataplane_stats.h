#ifndef TRAFFIC_GEN_DATAPLANE_STATS_H
#define TRAFFIC_GEN_DATAPLANE_STATS_H

/**
 * @file dataplane_stats.h
 * @brief Main-lcore packet counters, sampled timings, and NIC CSV reports.
 *
 * The main lcore owns all state and writes reports without synchronization.
 * Main metrics are cumulative; NIC fields report deltas between successful
 * snapshots. Worker-direct RX/TX activity is accounted for by worker metrics.
 */

#include <rte_ethdev.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/**
 * @brief Shared field order for metric storage, CSV headers, and CSV values.
 *
 * Packet, burst, poll, and drop counters cover all observed main-lcore work;
 * rx_burst_max retains the largest RX burst. Cycle fields and sampled_*
 * counters cover sampled loops only, so sampled packet counts must be used
 * when computing time per packet. Values are never reset by a report.
 */
#define TG_MAIN_METRIC_FIELDS(X)                                               \
        X(main_loops) X(sampled_loops) X(sampled_rx_packets)                    \
        X(sampled_tx_packets) X(main_loop_cycles) X(rx_burst_calls)             \
        X(rx_empty_bursts) X(rx_full_bursts) X(rx_packets) X(rx_burst_max)       \
        X(rx_burst_cycles) X(reassembly_cycles) X(dispatch_cycles)              \
        X(enqueue_cycles) X(reassembly_maintenance_cycles) X(tx_ring_polls)     \
        X(tx_ring_scan_cycles) X(tx_burst_calls) X(tx_packets)                  \
        X(tx_partial_bursts) X(tx_nic_drops) X(nic_tx_cycles)                   \
        X(stats_drain_cycles)

/** @brief Cumulative counters and timer-cycle sums owned by the main lcore. */
struct tg_main_metrics {
#define TG_MAIN_FIELD(name) uint64_t name;
        TG_MAIN_METRIC_FIELDS(TG_MAIN_FIELD)
#undef TG_MAIN_FIELD
};

/** @brief Optional CSV sink, sampling cadence, and NIC snapshot baseline. */
struct tg_dataplane_stats {
        struct tg_main_metrics main;
        /** Buffered output stream; NULL disables collection and reporting. */
        FILE *file;
        /** NIC port whose device-wide counters are sampled. */
        uint16_t port_id;
        /** Sample one loop in N; zero disables timing but keeps counters. */
        uint32_t sample_every;
        /** Unsampled loops remaining before the next sample. */
        uint32_t until_sample;
        /** Timer-cycle origin for the CSV elapsed_cycles column. */
        uint64_t start_cycles;
        /** Earliest timer timestamp for the next periodic report. */
        uint64_t next_report_cycles;
        /** Last successful NIC read, or start_cycles before the first one. */
        uint64_t nic_snapshot_cycles;
        /** Last successful NIC snapshot used to calculate interval deltas. */
        struct rte_eth_stats previous_nic;
        /** Whether previous_nic contains a successful read. */
        bool nic_baseline_valid;
        /** Latched CSV write error; suppresses further reports. */
        bool failed;
};

/**
 * @brief Initializes counters, opens the CSV sink, and captures a NIC baseline.
 * @param stats State to initialize; must not already own an open stream.
 * @param path Output path, truncated on open; NULL disables statistics.
 * @param port_id NIC port to query for device-wide statistics.
 * @param sample_every Timing sample interval; zero keeps only counters.
 * @return 0 on success (including disabled output), or -1 on file errors.
 *
 * A failed initial NIC read leaves the baseline invalid until a later read
 * succeeds; it does not prevent opening the report stream.
 */
int tg_dataplane_stats_open(struct tg_dataplane_stats *stats, const char *path,
                            uint16_t port_id, uint32_t sample_every);

/**
 * @brief Writes a cumulative main snapshot and NIC deltas at one-second cadence.
 * @param stats Main-lcore statistics and output state.
 * @param now Current timestamp in DPDK timer cycles.
 * @param final Bypasses the periodic deadline and labels the row as final.
 *
 * Disabled or failed sinks produce no output. Failed NIC reads or decreasing
 * counters produce zero NIC deltas with nic_delta_valid cleared. A successful
 * read becomes the next baseline even when it detects a counter reset.
 */
void tg_dataplane_stats_report(struct tg_dataplane_stats *stats, uint64_t now,
                               bool final);

/**
 * @brief Flushes and closes the sink without generating a final report.
 * @return 0 on success, or -1 for a prior write error or a close error.
 */
int tg_dataplane_stats_close(struct tg_dataplane_stats *stats);

/**
 * @brief Counts one main-loop iteration and selects whether to time it.
 *
 * An open sink is required even for loop counting. With sampling enabled,
 * the first loop is sampled, then every sample_every loops; one samples every
 * loop and zero disables timing. The countdown avoids hot-path division.
 * @return true when the caller should record this loop's stage timings.
 */
static inline bool tg_dataplane_sample(struct tg_dataplane_stats *stats) {
        if (stats->file == NULL)
                return false;
        stats->main.main_loops++;
        if (stats->sample_every == 0)
                return false;
        if (stats->until_sample != 0) {
                stats->until_sample--;
                return false;
        }
        stats->until_sample = stats->sample_every - 1;
        stats->main.sampled_loops++;
        return true;
}

#endif /* TRAFFIC_GEN_DATAPLANE_STATS_H */
