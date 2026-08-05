/**
 * @file reactor.c
 * @brief Implements bounded event draining followed by scheduler execution.
 *
 * Ready events are processed first because handlers can recycle flows and
 * release scheduler concurrency slots.  The scheduler then admits new work
 * within the same worker turn without delaying existing socket progress.
 */

#include "reactor.h"

#include <stddef.h>

/**
 * @copydoc tg_reactor_init
 */
void tg_reactor_init(struct tg_reactor *reactor, tg_reactor_tick_fn on_tick,
                     tg_reactor_event_fn on_event, void *ctx) {
        if (reactor == NULL)
                return;

        reactor->on_tick = on_tick;
        reactor->on_event = on_event;
        reactor->ctx = ctx;
}

/**
 * @copydoc tg_reactor_run
 */
void tg_reactor_run(void *ctx, unsigned int budget) {
        struct tg_reactor *reactor = ctx;
        struct owner_io_event events[32];
        unsigned int count;

        if (reactor == NULL || budget == 0)
                return;
        if (budget > 32)
                budget = 32;

        if (reactor->on_event != NULL) {
                count = owner_io_ready_burst(events, budget);
                for (unsigned int i = 0; i < count; i++)
                        reactor->on_event(reactor->ctx, &events[i]);
        }
        if (reactor->on_tick != NULL)
                reactor->on_tick(reactor->ctx, budget);
}
