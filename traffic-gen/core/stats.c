/**
 * @file stats.c
 * @brief Implements owner-local cumulative transaction accounting.
 *
 * Completion accounting runs before a flow is reset and returned to its pool,
 * allowing the request and response byte counters to be captured exactly
 * once.  No atomic operations are required because a shard has one writer.
 */

#include "stats.h"

#include <string.h>

/** @copydoc tg_stats_init */
void tg_stats_init(struct tg_stats *stats) {
        if (stats != NULL)
                memset(stats, 0, sizeof(*stats));
}

/** @copydoc tg_stats_on_admitted */
void tg_stats_on_admitted(struct tg_stats *stats) {
        if (stats == NULL)
                return;
        stats->txns_started++;
        stats->concurrency++;
}

/** @copydoc tg_stats_on_start_failure */
void tg_stats_on_start_failure(struct tg_stats *stats) {
        if (stats == NULL)
                return;
        stats->txns_started++;
        stats->txns_done++;
        stats->txns_fail++;
        stats->fail_connect++;
}

/** @copydoc tg_stats_on_flow_finished */
void tg_stats_on_flow_finished(struct tg_stats *stats,
                               const struct tg_flow *flow,
                               enum tg_flow_result result) {
        if (stats == NULL || flow == NULL)
                return;

        stats->txns_done++;
        if (stats->concurrency != 0)
                stats->concurrency--;
        stats->bytes_tx += flow->txn.request_offset;
        stats->bytes_rx += flow->txn.response_bytes;
        if (result == TG_FLOW_RESULT_SUCCESS) {
                stats->txns_success++;
                if (flow->txn.proto != NULL &&
                    strcmp(flow->txn.proto->name, "http") == 0)
                        stats->http_rps_total++;
                return;
        }

        stats->txns_fail++;
        switch (result) {
        case TG_FLOW_RESULT_CONNECT_FAILURE:
                stats->fail_connect++;
                break;
        case TG_FLOW_RESULT_PROTOCOL_FAILURE:
                stats->fail_proto++;
                break;
        case TG_FLOW_RESULT_IO_FAILURE:
        default:
                stats->fail_io++;
                break;
        }
}

/** @copydoc tg_stats_report_due */
bool tg_stats_report_due(struct tg_stats *stats, uint64_t now_cycles,
                         uint64_t cycles_per_second,
                         uint32_t report_interval_sec) {
        uint64_t interval;

        if (stats == NULL || cycles_per_second == 0 || report_interval_sec == 0)
                return false;
        interval = cycles_per_second * report_interval_sec;
        if (stats->last_report_cycles == 0) {
                stats->last_report_cycles = now_cycles;
                return false;
        }
        if (now_cycles < stats->last_report_cycles ||
            now_cycles - stats->last_report_cycles < interval)
                return false;
        stats->last_report_cycles = now_cycles;
        return true;
}
