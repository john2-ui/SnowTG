/** Open, paced arrivals with explicit bounded-backlog loss accounting. */
#include "scheduler.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static struct tg_phase_plan tg_phase(const struct tg_scheduler *s) {
        if (s->plan->phase_count != 0)
                return s->plan->phases[s->phase_index];
        /* Also support manually constructed single-phase plans. */
        struct tg_phase_plan phase = {.duration_sec = s->plan->duration_sec,
                                      .start_cps = s->plan->target_cps,
                                      .target_cps = s->plan->target_cps};
        return phase;
}

static uint32_t tg_shards(const struct tg_scheduler *s) {
        return s->plan->phase_count && s->plan->schedule_shard_count
                   ? s->plan->schedule_shard_count
                   : 1U;
}

/* Number of global arrivals strictly before t: ceil(integral(rate, 0, t)).
 * The exact integer integral avoids drift and per-turn fractional loss.
 * 128-bit intermediates cover max duration/rate and clocks through 10 GHz. */
static uint64_t tg_due_count(const struct tg_scheduler *s, uint64_t elapsed) {
        struct tg_phase_plan p = tg_phase(s);
        uint64_t duration = (uint64_t)p.duration_sec * s->cycles_per_second;
        __uint128_t rate_term = (__uint128_t)2 * p.start_cps * duration;
        if (p.target_cps >= p.start_cps)
                rate_term +=
                    (__uint128_t)(p.target_cps - p.start_cps) * elapsed;
        else
                rate_term -=
                    (__uint128_t)(p.start_cps - p.target_cps) * elapsed;
        __uint128_t numerator = (__uint128_t)elapsed * rate_term;
        __uint128_t denominator =
            (__uint128_t)2 * duration * s->cycles_per_second;
        uint64_t global =
            (uint64_t)((numerator + denominator - 1) / denominator);
        uint32_t shards = tg_shards(s);
        uint32_t index = shards == 1 ? 0 : s->plan->schedule_shard_index;
        return global <= index ? 0 : (global - index + shards - 1) / shards;
}

/* Invert the global rate integral for this shard's next arrival. Using the
 * global ordinal avoids rate-rounding drift when CPS is not divisible by workers. */
static uint64_t tg_due_cycles(const struct tg_scheduler *s) {
        struct tg_phase_plan p = tg_phase(s);
        uint32_t shards = tg_shards(s);
        uint64_t n = s->phase_consumed * shards +
                     (shards == 1 ? 0 : s->plan->schedule_shard_index);
        uint64_t offset;
        if (p.start_cps == p.target_cps) {
                offset = (uint64_t)((__uint128_t)n * s->cycles_per_second /
                                    p.target_cps);
        } else if (n == 0) {
                offset = 0;
        } else {
                /* Stable inverse of A(t)=a*t+(b-a)*t*t/(2*duration). */
                long double a = p.start_cps;
                long double slope =
                    ((long double)p.target_cps - a) / p.duration_sec;
                long double t =
                    (2.0L * n) / (a + sqrtl(a * a + 2.0L * slope * n));
                offset = (uint64_t)(t * s->cycles_per_second);
        }
        return s->phase_start_cycles + offset;
}

/* Count any-length skipped runs in O(classes), not O(missed requests).
 * Advance the same weighted selection cursor as real attempts so overload
 * cannot silently change the intended class mix. */
static void tg_skip(struct tg_scheduler *s, uint64_t count) {
        uint64_t total = s->plan->total_weight;
        uint64_t cursor = s->selection_cursor % total;
        uint64_t remainder = count % total;
        uint64_t begin = 0;
        for (uint32_t i = 0; i < s->plan->class_count; i++) {
                uint64_t end = begin + s->plan->classes[i].weight;
                uint64_t hits = (count / total) * (end - begin);
                for (unsigned int wrap = 0; wrap < 2; wrap++) {
                        uint64_t lo = begin + wrap * total;
                        uint64_t hi = end + wrap * total;
                        if (lo < cursor)
                                lo = cursor;
                        if (hi > cursor + remainder)
                                hi = cursor + remainder;
                        if (hi > lo)
                                hits += hi - lo;
                }
                s->counts[s->phase_index][i].skipped += hits;
                begin = end;
        }
        s->selection_cursor += count;
        s->phase_consumed += count;
        s->skipped_total += count;
}

static uint32_t tg_select(struct tg_scheduler *s) {
        uint32_t selected = s->selection_cursor++ % s->plan->total_weight;
        uint32_t cumulative = 0;
        for (uint32_t i = 0; i < s->plan->class_count; i++) {
                cumulative += s->plan->classes[i].weight;
                if (selected < cumulative)
                        return i;
        }
        return 0;
}

int tg_scheduler_init(struct tg_scheduler *s, const struct tg_plan *plan,
                      uint64_t hz) {
        if (s == NULL || plan == NULL || plan->class_count == 0 ||
            plan->class_count > TG_PLAN_MAX_CLASSES ||
            plan->total_weight == 0 || plan->max_concurrency == 0 ||
            plan->target_cps == 0 || plan->duration_sec == 0 ||
            plan->duration_sec > TG_PLAN_MAX_DURATION_SEC ||
            plan->phase_count > TG_PLAN_MAX_PHASES || hz == 0 ||
            hz > UINT64_C(10000000000)) {
                errno = EINVAL;
                return -1;
        }
        memset(s, 0, sizeof(*s));
        s->plan = plan;
        s->cycles_per_second = hz;
        s->selection_cursor = plan->selection_phase;
        return 0;
}

void tg_scheduler_start_at(struct tg_scheduler *s, uint64_t epoch) {
        s->started = true;
        s->start_cycles = epoch;
        s->phase_start_cycles = epoch;
        s->last_cycles = epoch;
        s->stop_cycles =
            epoch + (uint64_t)s->plan->duration_sec * s->cycles_per_second;
}

unsigned int tg_scheduler_tick(struct tg_scheduler *s, uint64_t now,
                               unsigned int budget, tg_scheduler_start_fn start,
                               void *ctx) {
        unsigned int attempts = 0;
        if (s == NULL || start == NULL || s->stopped)
                return 0;
        if (!s->started) {
                tg_scheduler_start_at(s, now);
                return 0;
        }
        if (now < s->start_cycles)
                return 0;
        if (now < s->last_cycles) {
                s->stopped = true;
                return 0;
        }
        s->last_cycles = now;
        for (;;) {
                struct tg_phase_plan phase = tg_phase(s);
                uint64_t duration =
                    (uint64_t)phase.duration_sec * s->cycles_per_second;
                uint64_t elapsed = now - s->phase_start_cycles;
                bool ended = elapsed >= duration;
                uint64_t due = tg_due_count(s, ended ? duration : elapsed);
                s->planned_total += due - s->phase_seen;
                s->phase_seen = due;
                if (!ended)
                        break;
                tg_skip(s, due - s->phase_consumed);
                s->global_arrival_offset +=
                    ((uint64_t)phase.duration_sec *
                         (phase.start_cps + phase.target_cps) +
                     1) /
                    2;
                s->phase_start_cycles += duration;
                s->phase_index++;
                s->phase_consumed = s->phase_seen = 0;
                s->token_numerator = 0;
                uint32_t phases =
                    s->plan->phase_count ? s->plan->phase_count : 1;
                if (s->phase_index == phases) {
                        s->stopped = true;
                        return 0;
                }
        }
        uint64_t pending = s->phase_seen - s->phase_consumed;
        if (pending > s->plan->max_concurrency) {
                tg_skip(s, pending - s->plan->max_concurrency);
                pending = s->plan->max_concurrency;
        }
        while (!s->resource_paused && attempts < budget && pending &&
               s->active < s->plan->max_concurrency) {
                uint32_t selected = tg_select(s);
                s->dispatch_planned_cycles = tg_due_cycles(s);
                /* Interleave shard-local consumed counts, including skipped arrivals.
                 * Dataset selection must not depend on worker completion order. */
                s->dispatch_ordinal =
                    s->global_arrival_offset +
                    s->phase_consumed * tg_shards(s) +
                    (tg_shards(s) == 1 ? 0 : s->plan->schedule_shard_index);
                s->counts[s->phase_index][selected].attempted++;
                s->phase_consumed++;
                if (start(ctx, &s->plan->classes[selected]) == 0)
                        s->active++;
                attempts++;
                pending--;
        }
        s->token_numerator = pending * s->cycles_per_second;
        return attempts;
}

/** @copydoc tg_scheduler_on_flow_finished */
void tg_scheduler_on_flow_finished(struct tg_scheduler *scheduler) {
        if (scheduler != NULL && scheduler->active != 0)
                scheduler->active--;
}

void tg_scheduler_on_socket_created(struct tg_scheduler *scheduler) {
        if (scheduler != NULL)
                scheduler->live_sockets++;
}

void tg_scheduler_on_socket_released(struct tg_scheduler *scheduler) {
        if (scheduler != NULL && scheduler->live_sockets != 0)
                scheduler->live_sockets--;
}

/** @copydoc tg_scheduler_set_resource_available */
void tg_scheduler_set_resource_available(struct tg_scheduler *scheduler,
                                         bool available) {
        if (scheduler == NULL)
                return;
        if (!available && !scheduler->resource_paused) {
                scheduler->resource_paused = true;
                scheduler->resource_pauses++;
        } else if (available) {
                scheduler->resource_paused = false;
        }
}

/** @copydoc tg_scheduler_is_stopped */
bool tg_scheduler_is_stopped(const struct tg_scheduler *scheduler) {
        return scheduler != NULL && scheduler->stopped;
}
