/**
 * @file scheduler.c
 * @brief Implements token-bucket admission and weighted class selection.
 *
 * All arithmetic is integer based: token credit is stored in cycle-clock
 * units to avoid rate loss from fractional tokens during frequent reactor
 * turns.  Credit is capped to one concurrency window to prevent unbounded
 * bursts after a stalled worker.
 */

#include "scheduler.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

/**
 * @brief Selects the next class using deterministic weighted round-robin.
 *
 * Advancing the cursor before each lookup gives reproducible sequences for a
 * fixed plan and tick trace, which is important for scenario unit tests.
 */
static const struct tg_class_plan *
tg_scheduler_select_class(struct tg_scheduler *scheduler) {
        uint32_t selected = (uint32_t)(scheduler->selection_cursor %
                                       scheduler->plan->total_weight);
        uint32_t cumulative = 0;

        scheduler->selection_cursor++;
        for (uint32_t i = 0; i < scheduler->plan->class_count; i++) {
                cumulative += scheduler->plan->classes[i].weight;
                if (selected < cumulative)
                        return &scheduler->plan->classes[i];
        }
        return NULL;
}

/**
 * @brief Accrues elapsed CPS credit while saturating at the burst cap.
 * @param scheduler Scheduler whose token numerator is updated.
 * @param elapsed_cycles Cycle-clock time since the previous scheduler turn.
 */
static void tg_scheduler_add_tokens(struct tg_scheduler *scheduler,
                                    uint64_t elapsed_cycles) {
        uint64_t cap;
        uint64_t added;

        cap = (uint64_t)scheduler->plan->max_concurrency *
              scheduler->cycles_per_second;
        if (elapsed_cycles > UINT64_MAX / scheduler->plan->target_cps)
                added = cap;
        else
                added = elapsed_cycles * scheduler->plan->target_cps;
        if (added >= cap - scheduler->token_numerator)
                scheduler->token_numerator = cap;
        else
                scheduler->token_numerator += added;
}

/** @copydoc tg_scheduler_init */
int tg_scheduler_init(struct tg_scheduler *scheduler,
                      const struct tg_plan *plan, uint64_t cycles_per_second) {
        if (scheduler == NULL || plan == NULL || plan->class_count == 0 ||
            plan->total_weight == 0 || plan->max_concurrency == 0 ||
            plan->target_cps == 0 || cycles_per_second == 0) {
                errno = EINVAL;
                return -1;
        }
        memset(scheduler, 0, sizeof(*scheduler));
        scheduler->plan = plan;
        scheduler->cycles_per_second = cycles_per_second;
        return 0;
}

/** @copydoc tg_scheduler_tick */
unsigned int tg_scheduler_tick(struct tg_scheduler *scheduler,
                               uint64_t now_cycles, unsigned int budget,
                               tg_scheduler_start_fn start, void *start_ctx) {
        unsigned int started = 0;

        if (scheduler == NULL || start == NULL || budget == 0 ||
            scheduler->stopped)
                return 0;
        if (!scheduler->started) {
                scheduler->started = true;
                scheduler->start_cycles = now_cycles;
                scheduler->last_cycles = now_cycles;
                return 0;
        }
        if (now_cycles < scheduler->last_cycles ||
            now_cycles - scheduler->start_cycles >=
                (uint64_t)scheduler->plan->duration_sec *
                    scheduler->cycles_per_second) {
                scheduler->stopped = true;
                return 0;
        }

        tg_scheduler_add_tokens(scheduler, now_cycles - scheduler->last_cycles);
        scheduler->last_cycles = now_cycles;
        while (started < budget &&
               scheduler->active < scheduler->plan->max_concurrency &&
               scheduler->token_numerator >= scheduler->cycles_per_second) {
                const struct tg_class_plan *class_plan =
                    tg_scheduler_select_class(scheduler);

                scheduler->token_numerator -= scheduler->cycles_per_second;
                if (class_plan == NULL)
                        break;
                if (start(start_ctx, class_plan) == 0)
                        scheduler->active++;
                started++;
        }
        return started;
}

/** @copydoc tg_scheduler_on_flow_finished */
void tg_scheduler_on_flow_finished(struct tg_scheduler *scheduler) {
        if (scheduler != NULL && scheduler->active != 0)
                scheduler->active--;
}

/** @copydoc tg_scheduler_is_stopped */
bool tg_scheduler_is_stopped(const struct tg_scheduler *scheduler) {
        return scheduler != NULL && scheduler->stopped;
}
