#include "latency.h"
#include "workflow.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* 32 exact buckets + 59 exponents (5..63) * 16 slots = 976 buckets.
 * Slots in [2^e,2^(e+1)) have width 2^(e-4); the top slot ends at UINT64_MAX. */
static unsigned int tg_hist_index(uint64_t us) {
        if (us < 32)
                return (unsigned int)us;
        unsigned int exponent = 63U - (unsigned int)__builtin_clzll(us);
        return 32U + (exponent - 5U) * 16U +
               (unsigned int)(us >> (exponent - 4U)) - 16U;
}

static uint64_t tg_hist_upper(unsigned int index) {
        if (index < 32)
                return index;
        unsigned int exponent = (index - 32U) / 16U + 5U;
        unsigned int slot = (index - 32U) % 16U;
        if (exponent == 63 && slot == 15)
                return UINT64_MAX;
        return ((uint64_t)(17U + slot) << (exponent - 4U)) - 1U;
}

void tg_histogram_record(struct tg_histogram *hist, uint64_t us) {
        hist->buckets[tg_hist_index(us)]++;
        hist->count++;
        if (us > hist->max_us)
                hist->max_us = us;
}

void tg_histogram_merge(struct tg_histogram *dst,
                        const struct tg_histogram *src) {
        for (unsigned int i = 0; i < TG_HIST_BUCKETS; i++)
                dst->buckets[i] += src->buckets[i];
        dst->count += src->count;
        if (src->max_us > dst->max_us)
                dst->max_us = src->max_us;
}

uint64_t tg_histogram_quantile(const struct tg_histogram *hist,
                               unsigned int permille) {
        if (hist->count == 0 || permille > 1000 || permille == 0)
                return 0;
        uint64_t rank =
            (uint64_t)(((__uint128_t)hist->count * permille + 999) / 1000);
        uint64_t sum = 0;
        for (unsigned int i = 0; i < TG_HIST_BUCKETS; i++) {
                sum += hist->buckets[i];
                if (sum >= rank) {
                        uint64_t upper = tg_hist_upper(i);
                        return upper < hist->max_us ? upper : hist->max_us;
                }
        }
        return hist->max_us;
}

static uint64_t tg_us(uint64_t cycles, uint64_t hz) {
        /* Round upward so a nonzero sub-microsecond interval remains visible. */
        return (uint64_t)(((__uint128_t)cycles * 1000000U + hz - 1) / hz);
}

int tg_latency_init(struct tg_latency *latency, const struct tg_plan *plan,
                    uint64_t hz) {
        if (!latency || !plan || !plan->class_count || !plan->phase_count ||
            !hz) {
                errno = EINVAL;
                return -1;
        }
        memset(latency, 0, sizeof(*latency));
        latency->phase_count = plan->phase_count;
        latency->class_count = plan->class_count;
        latency->hz = hz;
        latency->groups = calloc((size_t)plan->phase_count * plan->class_count,
                                 sizeof(*latency->groups));
        return latency->groups ? 0 : -1;
}

void tg_latency_fini(struct tg_latency *latency) {
        free(latency->groups);
        memset(latency, 0, sizeof(*latency));
}

static struct tg_latency_group *tg_group(struct tg_latency *l, uint32_t phase,
                                         uint32_t cls) {
        if (!l->groups || phase >= l->phase_count || cls >= l->class_count)
                return NULL;
        return &l->groups[phase * l->class_count + cls];
}

void tg_latency_on_admitted(struct tg_latency *l, struct tg_flow *flow,
                            uint32_t phase, uint32_t cls, uint64_t planned) {
        flow->planned_cycles = planned;
        flow->load_phase_index = phase;
        flow->class_index = cls;
        struct tg_latency_group *g = tg_group(l, phase, cls);
        if (!g)
                return;
        uint64_t actual = flow->start_cycles;
        if (g->admitted == 0) {
                g->planned_first = planned;
                g->actual_first = actual;
        }
        g->planned_last = planned;
        g->actual_last = actual;
        g->admitted++;
        tg_histogram_record(
            &g->hist[TG_LAT_SCHEDULE],
            tg_us(actual >= planned ? actual - planned : 0, l->hz));
}

void tg_latency_on_start_failed(struct tg_latency *l, uint32_t phase,
                                uint32_t cls) {
        struct tg_latency_group *g = tg_group(l, phase, cls);
        if (g)
                g->start_failed++;
}

void tg_latency_on_finished(struct tg_latency *l, const struct tg_flow *flow,
                            enum tg_flow_result result, uint64_t now) {
        struct tg_latency_group *g =
            tg_group(l, flow->load_phase_index, flow->class_index);
        if (!g)
                return;
        if (result == TG_FLOW_RESULT_SUCCESS)
                g->success++;
        else
                g->failed++;
        if (flow->connected_cycles &&
            flow->connected_cycles >= flow->start_cycles)
                tg_histogram_record(
                    &g->hist[TG_LAT_CONNECT],
                    tg_us(flow->connected_cycles - flow->start_cycles, l->hz));
        if (flow->first_rx_cycles &&
            flow->first_rx_cycles >= flow->start_cycles)
                tg_histogram_record(
                    &g->hist[TG_LAT_FIRST_RX],
                    tg_us(flow->first_rx_cycles - flow->start_cycles, l->hz));
        if (now >= flow->start_cycles) {
                uint64_t us = tg_us(now - flow->start_cycles, l->hz);
                tg_histogram_record(&g->hist[TG_LAT_COMPLETE], us);
                tg_histogram_record(&g->hist[result == TG_FLOW_RESULT_SUCCESS
                                                 ? TG_LAT_COMPLETE_SUCCESS
                                                 : TG_LAT_COMPLETE_FAILURE],
                                    us);
        }
        if (now >= flow->planned_cycles)
                tg_histogram_record(&g->hist[TG_LAT_SCHEDULED_COMPLETE],
                                    tg_us(now - flow->planned_cycles, l->hz));
}

void tg_latency_on_drained(struct tg_latency *l, uint64_t stop, uint64_t now,
                           bool clean) {
        if (!l->groups)
                return;
        l->drained = clean;
        l->drain_us = now >= stop ? tg_us(now - stop, l->hz) : 0;
}

static void tg_group_merge(struct tg_latency_group *dst,
                           const struct tg_latency_group *src) {
        for (unsigned int m = 0; m < TG_LAT_METRICS; m++)
                tg_histogram_merge(&dst->hist[m], &src->hist[m]);
        if (src->admitted) {
                if (!dst->admitted || src->planned_first < dst->planned_first)
                        dst->planned_first = src->planned_first;
                if (!dst->admitted || src->actual_first < dst->actual_first)
                        dst->actual_first = src->actual_first;
                if (src->planned_last > dst->planned_last)
                        dst->planned_last = src->planned_last;
                if (src->actual_last > dst->actual_last)
                        dst->actual_last = src->actual_last;
        }
        dst->admitted += src->admitted;
        dst->success += src->success;
        dst->failed += src->failed;
        dst->start_failed += src->start_failed;
}

/* User-supplied class/phase names may contain commas. Quote all text fields. */
static void tg_csv_text(FILE *f, const char *text) {
        fputc('"', f);
        for (; *text; text++) {
                if (*text == '"')
                        fputc('"', f);
                fputc(*text, f);
        }
        fputc('"', f);
}

static void tg_csv_row(FILE *f, const char *scope, int worker,
                       const char *phase, const char *protocol, const char *cls,
                       const char *metric, const struct tg_histogram *hist,
                       const struct tg_latency_group *g, uint64_t attempted,
                       uint64_t skipped, uint64_t epoch, uint64_t hz) {
        tg_csv_text(f, scope);
        fprintf(f, ",%d,", worker);
        tg_csv_text(f, phase);
        fputc(',', f);
        tg_csv_text(f, protocol);
        fputc(',', f);
        tg_csv_text(f, cls);
        fputc(',', f);
        tg_csv_text(f, metric);
        fprintf(
            f,
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64,
            attempted + skipped, attempted, skipped, g->admitted, g->success,
            g->failed, g->start_failed, hist->count,
            tg_histogram_quantile(hist, 500), tg_histogram_quantile(hist, 900),
            tg_histogram_quantile(hist, 950), tg_histogram_quantile(hist, 990),
            tg_histogram_quantile(hist, 999), hist->max_us);
        if (g->admitted && strcmp(scope, "step") != 0) {
                fprintf(f, ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64,
                        tg_us(g->planned_first - epoch, hz),
                        tg_us(g->planned_last - epoch, hz),
                        tg_us(g->actual_first - epoch, hz),
                        tg_us(g->actual_last - epoch, hz));
        } else
                fputs(",,,,", f);
        /* Sparse upper_bound_us:count pairs make exact cross-worker merging
         * auditable. */
        fputs(",\"", f);
        bool first = true;
        for (unsigned int i = 0; i < TG_HIST_BUCKETS; i++)
                if (hist->buckets[i]) {
                        fprintf(f, "%s%" PRIu64 ":%" PRIu64, first ? "" : ";",
                                tg_hist_upper(i), hist->buckets[i]);
                        first = false;
                }
        fputs("\"", f);
}

static const char *const tg_metric_names[] = {
    "schedule",           "connect",          "first_rx",        "complete",
    "scheduled_complete", "complete_success", "complete_failure"};

static void tg_write_group(FILE *f, const char *scope, int worker,
                           const char *phase, const char *proto,
                           const char *cls, const struct tg_latency_group *g,
                           uint64_t attempts, uint64_t skipped, uint64_t epoch,
                           uint64_t hz) {
        for (unsigned int m = 0; m < TG_LAT_METRICS; m++) {
                tg_csv_row(f, scope, worker, phase, proto, cls,
                           tg_metric_names[m], &g->hist[m], g, attempts,
                           skipped, epoch, hz);
                fputs(",,0,0,0,0\n", f);
        }
}

int tg_latency_csv_write(FILE *f, const struct tg_plan *plan,
                         const struct tg_latency *const *workers,
                         const struct tg_scheduler *const *schedulers,
                         unsigned int count) {
        fputs("scope,worker,load_phase,protocol,class,metric,planned,attempted,"
              "skipped,"
              "admitted,success,failed,start_failed,samples,p50_us,p90_us,p95_"
              "us,p99_us,"
              "p999_us,max_us,planned_first_us,planned_last_us,actual_first_us,"
              "actual_last_us,buckets,step,reached,started,branch_skipped,not_"
              "reached\n",
              f);
        struct tg_latency_group *merged = calloc(1, sizeof(*merged));
        if (!merged)
                return -1;
        uint64_t epoch = schedulers[0]->start_cycles;
        uint64_t hz = workers[0]->hz;
        for (uint32_t p = 0; p < plan->phase_count; p++) {
                for (uint32_t c = 0; c < plan->class_count; c++) {
                        uint64_t attempts = 0, skipped = 0;
                        memset(merged, 0, sizeof(*merged));
                        for (unsigned int w = 0; w < count; w++) {
                                const struct tg_latency_group *g =
                                    &workers[w]
                                         ->groups[p * plan->class_count + c];
                                const struct tg_schedule_count *n =
                                    &schedulers[w]->counts[p][c];
                                tg_write_group(
                                    f, "worker", (int)w, plan->phases[p].name,
                                    plan->classes[c].proto->name,
                                    plan->classes[c].name, g, n->attempted,
                                    n->skipped, epoch, hz);
                                tg_group_merge(merged, g);
                                attempts += n->attempted;
                                skipped += n->skipped;
                        }
                        tg_write_group(f, "class", -1, plan->phases[p].name,
                                       plan->classes[c].proto->name,
                                       plan->classes[c].name, merged, attempts,
                                       skipped, epoch, hz);
                }
                /* Protocol rows merge all matching classes and workers. */
                for (uint32_t c = 0; c < plan->class_count; c++) {
                        bool seen = false;
                        for (uint32_t earlier = 0; earlier < c; earlier++)
                                if (plan->classes[earlier].proto ==
                                    plan->classes[c].proto)
                                        seen = true;
                        if (seen)
                                continue;
                        uint64_t attempts = 0, skipped = 0;
                        memset(merged, 0, sizeof(*merged));
                        for (uint32_t k = c; k < plan->class_count; k++) {
                                if (plan->classes[k].proto !=
                                    plan->classes[c].proto)
                                        continue;
                                for (unsigned int w = 0; w < count; w++) {
                                        tg_group_merge(
                                            merged,
                                            &workers[w]->groups
                                                 [p * plan->class_count + k]);
                                        attempts += schedulers[w]
                                                        ->counts[p][k]
                                                        .attempted;
                                        skipped +=
                                            schedulers[w]->counts[p][k].skipped;
                                }
                        }
                        tg_write_group(f, "protocol", -1, plan->phases[p].name,
                                       plan->classes[c].proto->name, "all",
                                       merged, attempts, skipped, epoch, hz);
                }
        }
        /* Drain is a run-wide resource barrier, not a transaction/class sample.
         * The global barrier is max(worker completion time), never mean/P99 of
         * workers. */
        uint64_t drain = 0;
        bool clean = true;
        for (unsigned int w = 0; w <= count; w++) {
                memset(merged, 0, sizeof(*merged));
                if (w < count) {
                        clean &= workers[w]->drained;
                        if (workers[w]->drain_us > drain)
                                drain = workers[w]->drain_us;
                        if (workers[w]->drained)
                                tg_histogram_record(&merged->hist[0],
                                                    workers[w]->drain_us);
                        else
                                merged->failed = 1;
                } else if (clean)
                        tg_histogram_record(&merged->hist[0], drain);
                else
                        merged->failed = 1;
                tg_csv_row(f, w < count ? "worker" : "run",
                           w < count ? (int)w : -1, "drain", "all", "all",
                           "drain", &merged->hist[0], merged, 0, 0, epoch, hz);
                fputs(",,0,0,0,0\n", f);
        }
        free(merged);
        return ferror(f) ? -1 : 0;
}

/**
 * @brief Writes one merged step histogram using the common CSV representation.
 *
 * Internal failed includes start_failed; split them so CSV consumers can enforce
 * reached = started + start_failed and started = success + failed independently.
 * @return 0 on success; -1 on allocation or stream failure.
 */
int tg_latency_csv_step(FILE *f, const char *phase, const char *cls,
                        const char *protocol, const char *step,
                        const char *metric, const struct tg_histogram *hist,
                        const struct tg_step_counts *n) {
        struct tg_latency_group *g = calloc(1, sizeof(*g));
        if (!g)
                return -1;
        /* No admission timestamp range is exported for merged step rows. */
        g->admitted = n->started;
        g->success = n->success;
        g->failed = n->failed - n->start_failed;
        g->start_failed = n->start_failed;
        tg_csv_row(f, "step", -1, phase, protocol, cls, metric, hist, g,
                   n->reached, 0, 0, 1);
        fputc(',', f);
        tg_csv_text(f, step);
        fprintf(f, ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                n->reached, n->started, n->branch_skipped, n->not_reached);
        free(g);
        return ferror(f) ? -1 : 0;
}
