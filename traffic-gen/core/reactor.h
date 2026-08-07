#ifndef TRAFFIC_GEN_REACTOR_H
#define TRAFFIC_GEN_REACTOR_H

/**
 * @file reactor.h
 * @brief Defines the owner-worker reactor bridge for traffic generation.
 *
 * The reactor consumes readiness notifications produced by the owner I/O
 * subsystem, then runs one bounded scheduling tick.  This ordering releases
 * completed flows before the scheduler evaluates its concurrency limit.
 */

#include "../../pro-stack/owner_io.h"

#include <stdint.h>

/**
 * @brief Invoked once after the current ready-event burst is drained.
 * @param ctx Opaque context supplied when the reactor is initialized.
 * @param budget Maximum amount of work permitted during this worker turn.
 */
typedef void (*tg_reactor_tick_fn)(void *ctx, unsigned int budget);

/**
 * @brief Delivers one readiness notification to the owning application.
 * @param ctx Opaque context supplied when the reactor is initialized.
 * @param event Immutable event describing socket readiness.
 */
typedef void (*tg_reactor_event_fn)(void *ctx,
                                    const struct owner_io_event *event);

/**
 * @brief Callbacks and context used by an owner worker turn.
 *
 * All members are owner-lcore local.  Callback implementations may complete
 * and recycle a flow while handling an event, so no event-pointer state is
 * retained after the callback returns.
 */
struct tg_reactor {
        tg_reactor_event_fn on_event;
        tg_reactor_tick_fn on_tick;
        void *ctx;
        /** Counters reset by tg_reactor_metrics_take() after each report. */
        uint64_t turns;  /**< Owner-worker reactor invocations. */
        uint64_t events; /**< Ready events delivered to flows. */
        uint32_t event_burst_high_water; /**< Largest one-turn ready burst. */
};

/**
 * @brief Initializes a reactor with its event and scheduling callbacks.
 * @param reactor Reactor to initialize.
 * @param on_tick Callback run after ready events have been handled.
 * @param on_event Callback that handles each ready event.
 * @param ctx Shared opaque context passed to both callbacks.
 */
void tg_reactor_init(struct tg_reactor *reactor, tg_reactor_tick_fn on_tick,
                     tg_reactor_event_fn on_event, void *ctx);

/**
 * @brief Executes one bounded owner-worker reactor turn.
 * @param ctx Pointer to the initialized @ref tg_reactor.
 * @param budget Maximum number of ready events and scheduling attempts.
 */
void tg_reactor_run(void *ctx, unsigned int budget);
/**
 * Return and clear reactor activity accumulated since the prior call.
 * The reactor and caller must execute on the same owner lcore.
 */
void tg_reactor_metrics_take(struct tg_reactor *reactor, uint64_t *turns,
                             uint64_t *events, uint32_t *burst_high_water);

#endif /* TRAFFIC_GEN_REACTOR_H */
