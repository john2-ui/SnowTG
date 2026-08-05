#ifndef TRAFFIC_GEN_SCHEDULER_H
#define TRAFFIC_GEN_SCHEDULER_H

/**
 * @file scheduler.h
 * @brief Owner-local CPS and concurrency scheduler for immutable plans.
 *
 * The scheduler uses an integer token numerator to preserve fractional CPS
 * credit between short reactor turns.  It has no socket knowledge; callers
 * provide the callback that admits the selected traffic class.
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
 * Failed attempts still consume a scheduler token because CPS limits attempt
 * rate, rather than only successful connections.
 */
typedef int (*tg_scheduler_start_fn)(void *ctx,
                                     const struct tg_class_plan *class_plan);

/**
 * @brief Mutable scheduling state owned exclusively by one worker lcore.
 *
 * @p token_numerator represents tokens with @p cycles_per_second as its
 * denominator.  @p active is incremented only for successfully admitted
 * flows and must be decremented exactly once by the completion observer.
 */
struct tg_scheduler {
        const struct tg_plan *plan;
        uint64_t cycles_per_second;
        uint64_t start_cycles;
        uint64_t last_cycles;
        uint64_t token_numerator;
        uint64_t selection_cursor;
        uint32_t active;
        uint32_t live_sockets;
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

/**
 * @brief Adds elapsed CPS credit and starts bounded eligible transactions.
 * @param scheduler Owner-local scheduler state.
 * @param now_cycles Monotonic cycle-clock timestamp for this worker turn.
 * @param budget Maximum start attempts to issue during this turn.
 * @param start Callback that creates the selected transaction.
 * @param start_ctx Opaque context forwarded to @p start.
 * @return Number of attempted starts, including callback failures.
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
 * @brief Reports whether plan duration has permanently stopped admissions.
 * @param scheduler Scheduler to inspect.
 * @return @c true after duration expiry or a backwards clock observation.
 */
bool tg_scheduler_is_stopped(const struct tg_scheduler *scheduler);

#endif /* TRAFFIC_GEN_SCHEDULER_H */
