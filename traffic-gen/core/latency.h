#ifndef TRAFFIC_GEN_LATENCY_H
#define TRAFFIC_GEN_LATENCY_H

#include "flow.h"
#include "scheduler.h"
#include <stdio.h>

/* Owner-local software timing: all inputs use the scheduler's cycle clock;
 * exported durations are rounded up to microseconds. No hot-path allocation
 * or synchronization is needed after init. Main reads only after workers join.
 */
/* Exact 0..31 us; thereafter 16 subdivisions per power of two (<=6.25%
 * upper-bound quantile error). Covers uint64_t microseconds without clipping.
 */
#define TG_HIST_BUCKETS 976U
struct tg_histogram {
        uint64_t buckets[TG_HIST_BUCKETS];
        uint64_t count;
        uint64_t max_us;
};
void tg_histogram_record(struct tg_histogram *hist, uint64_t us);
void tg_histogram_merge(struct tg_histogram *dst,
                        const struct tg_histogram *src);
/* Nearest rank, permille in [1,1000]; bucket upper bound capped at max_us.
 * Empty input/invalid permille returns 0: consult count before treating it as
 * a measured zero. Merge bins before computing cross-worker percentiles. */
uint64_t tg_histogram_quantile(const struct tg_histogram *hist,
                               unsigned int permille);

enum tg_latency_metric {
        /* planned -> admitted; the remaining metrics start at admission,
         * except scheduled_complete, which includes scheduling delay. */
        TG_LAT_SCHEDULE,
        TG_LAT_CONNECT,
        TG_LAT_FIRST_RX,
        TG_LAT_COMPLETE,
        TG_LAT_SCHEDULED_COMPLETE,
        TG_LAT_COMPLETE_SUCCESS,
        TG_LAT_COMPLETE_FAILURE,
        TG_LAT_METRICS
};
struct tg_latency_group {
        struct tg_histogram hist[TG_LAT_METRICS];
        uint64_t admitted, success, failed, start_failed;
        uint64_t planned_first, planned_last, actual_first, actual_last; /* cycles */
};
struct tg_latency {
        /* Allocated only with --latency-csv; one writer per worker. */
        struct tg_latency_group *groups;
        uint32_t phase_count, class_count;
        uint64_t hz;
        uint64_t drain_us;
        bool drained;
};
/* Allocate phase_count * class_count groups for a valid compiled plan.
 * Destination must not own an earlier allocation; fini releases and clears it.
 * Returns 0 on success, -1 on invalid arguments/allocation failure. */
int tg_latency_init(struct tg_latency *latency, const struct tg_plan *plan,
                    uint64_t hz);
void tg_latency_fini(struct tg_latency *latency);
/* Call on every admitted transaction, including Keep-Alive reuse, before it
 * can finish. Its planned phase/class attribution survives phase transitions. */
void tg_latency_on_admitted(struct tg_latency *latency, struct tg_flow *flow,
                            uint32_t phase, uint32_t class_index,
                            uint64_t planned_cycles);
/* Exactly one terminal callback per admission, before recycling the flow.
 * Missing connect/first-byte events contribute no sample, not a zero latency.
 * Skipped/start-failed arrivals have counters but no response-time samples. */
void tg_latency_on_finished(struct tg_latency *latency,
                            const struct tg_flow *flow,
                            enum tg_flow_result result, uint64_t now);
void tg_latency_on_start_failed(struct tg_latency *latency, uint32_t phase,
                                uint32_t class_index);
/* stop is the scheduled end of offered load. clean requires all worker-owned
 * flows/sockets/TCP objects returned; forced cleanup is not a clean sample. */
void tg_latency_on_drained(struct tg_latency *latency, uint64_t stop,
                           uint64_t now, bool clean);
/* Called only on Main after joining every worker. CSV includes mergeable bins,
 * not averages of worker percentiles. Counts repeat on each metric row; worker,
 * class and protocol scopes overlap and must not be summed together.
 * Workers must have initialized collectors with the same plan, clock and epoch.
 * The caller owns the open file; returns -1 on allocation/write failure. */
int tg_latency_csv_write(FILE *file, const struct tg_plan *plan,
                         const struct tg_latency *const *workers,
                         const struct tg_scheduler *const *schedulers,
                         unsigned int worker_count);
#endif
