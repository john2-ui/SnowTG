#ifndef TRAFFIC_GEN_SCHEDULER_H
#define TRAFFIC_GEN_SCHEDULER_H

/**
 * @file scheduler.h
 * @brief Owner-local CPS and concurrency scheduler for immutable plans.
 *
 * Open arrivals follow a fixed clock, independent of completions. Pending
 * arrivals are bounded by concurrency; overflow and phase-end misses are
 * counted explicitly. Callers provide the transaction admission callback.
 */

#include "scenario.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Attempts to start one transaction for a selected class.
 * @param ctx Opaque caller context supplied to @ref tg_scheduler_tick.
 * @param class_plan Immutable class selected by weighted round-robin.
 * @return 0 if a flow was admitted; nonzero if the start attempt failed.
 *
 * Each attempted arrival is consumed once, even if admission fails.
 */
typedef int (*tg_scheduler_start_fn)(void *ctx,
                                     const struct tg_class_plan *class_plan);

struct tg_schedule_count {
        uint64_t attempted;
        uint64_t skipped;
};

/**
 * @brief Mutable scheduling state owned exclusively by one worker lcore.
 *
 * @p token_numerator exposes pending arrivals times clock frequency for
 * compatibility with the existing token gauge, not fractional rate tokens.
 * @p selection_cursor starts at the plan's shard-specific weighted-round-robin
 * phase. @p active is incremented only for successfully
 * admitted flows and must be decremented exactly once by the completion
 * observer.
 */
struct tg_scheduler {
        const struct tg_plan *plan;
        uint64_t cycles_per_second;
        uint64_t start_cycles;
        uint64_t last_cycles;
        uint64_t token_numerator;
        uint64_t selection_cursor;
        uint64_t phase_start_cycles;
        /* Per-shard current-phase arrivals: consumed = attempted + skipped;
         * seen = due by last tick; their difference is the pending backlog. */
        /* Total planned arrivals in completed phases, shared in meaning across shards. */
        uint64_t global_arrival_offset;
        uint64_t dispatch_ordinal; /**< Global plan ordinal, including skipped
                                      arrivals. */
        uint64_t phase_consumed;
        uint64_t phase_seen;
        uint64_t planned_total;
        uint64_t skipped_total;
        uint64_t dispatch_planned_cycles; /* Selected deadline, valid in start callback. */
        uint64_t stop_cycles;
        uint32_t phase_index;
        struct tg_schedule_count counts[TG_PLAN_MAX_PHASES][TG_PLAN_MAX_CLASSES];
        uint32_t active;
        uint32_t live_sockets;
        uint64_t resource_pauses;
        bool resource_paused;
        bool started;
        bool stopped;
};

/**
 * @brief Initializes scheduler state for an immutable compiled plan.
 * @param scheduler Destination scheduler.
 * @param plan Valid plan that remains alive for the scheduler lifetime.
 * @param cycles_per_second Frequency of the clock passed to tick().
 * @return 0 on success; -1 with @c errno set to @c EINVAL otherwise.
 */
int tg_scheduler_init(struct tg_scheduler *scheduler,
                      const struct tg_plan *plan, uint64_t cycles_per_second);

/** Set a shared epoch before workers launch; ticks before it admit nothing. */
void tg_scheduler_start_at(struct tg_scheduler *scheduler, uint64_t epoch);

/**
 * @brief Accounts clock-driven arrivals and starts bounded eligible transactions.
 * @param scheduler Owner-local scheduler state.
 * @param now_cycles Monotonic cycle-clock timestamp for this worker turn.
 * @param budget Maximum start attempts to issue during this turn.
 * @param start Callback that creates the selected transaction.
 * @param start_ctx Opaque context forwarded to @p start.
 * @return Number of attempted starts, including callback failures.
 *
 * Zero budget or a resource pause suppresses admission, not clock accounting.
 * Overflow drops the oldest pending arrivals; phase-end backlog is skipped
 * instead of replayed at the next phase's rate.
 */
unsigned int tg_scheduler_tick(struct tg_scheduler *scheduler,
                               uint64_t now_cycles, unsigned int budget,
                               tg_scheduler_start_fn start, void *start_ctx);

/**
 * @brief Releases one active-concurrency slot after flow completion.
 * @param scheduler Owner-local scheduler that admitted the completed flow.
 */
void tg_scheduler_on_flow_finished(struct tg_scheduler *scheduler);

/** Records allocation of a socket that remains live through TCP teardown. */
void tg_scheduler_on_socket_created(struct tg_scheduler *scheduler);

/** Releases a socket-lifecycle slot after nsock_free() finishes. */
void tg_scheduler_on_socket_released(struct tg_scheduler *scheduler);

/**
 * Apply owner-local resource hysteresis before attempting admissions.
 * @p available must be false below low water and true only above high water.
 */
void tg_scheduler_set_resource_available(struct tg_scheduler *scheduler,
                                         bool available);

/**
 * @brief Reports whether plan duration has permanently stopped admissions.
 * @param scheduler Scheduler to inspect.
 * @return @c true after duration expiry or a backwards clock observation.
 */
bool tg_scheduler_is_stopped(const struct tg_scheduler *scheduler);

#endif /* TRAFFIC_GEN_SCHEDULER_H */
