#ifndef TRAFFIC_GEN_REACTOR_H
#define TRAFFIC_GEN_REACTOR_H

#include "../pro-stack/owner_io.h"

/**
 * @file reactor.h
 * @brief Owner-local traffic-generator scheduling and readiness dispatch.
 */

/**
 * Run per-owner scheduling work once per packet-worker turn.
 *
 * The callback runs on the socket owner lcore.  It may create flows and call
 * owner_io_* APIs, subject to @p budget.
 */
typedef void (*tg_reactor_tick_fn)(void *ctx, unsigned int budget);

/** Consume one owner-local readiness notification in the flow scheduler. */
typedef void (*tg_reactor_event_fn)(void *ctx,
                                    const struct owner_io_event *event);

/** Minimal bridge between the stack-ready queue and a traffic-generator flow.
 */
struct tg_reactor {
        tg_reactor_event_fn on_event;
        tg_reactor_tick_fn on_tick;
        void *ctx;
};

/** Initialize a reactor with its event consumer and opaque scheduler context.
 */
void tg_reactor_init(struct tg_reactor *reactor, tg_reactor_tick_fn on_tick,
                     tg_reactor_event_fn on_event, void *ctx);
/** Drain a bounded batch of owner-local readiness events for one worker turn.
 */
void tg_reactor_run(void *ctx, unsigned int budget);

#endif /* TRAFFIC_GEN_REACTOR_H */
