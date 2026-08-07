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

        reactor->turns = 0;
        reactor->events = 0;
        reactor->event_burst_high_water = 0;
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

        reactor->turns++;
        if (reactor->on_event != NULL) {
                count = owner_io_ready_burst(events, budget);
                reactor->events += count;
                if (count > reactor->event_burst_high_water)
                        reactor->event_burst_high_water = count;
                for (unsigned int i = 0; i < count; i++)
                        reactor->on_event(reactor->ctx, &events[i]);
        }
        if (reactor->on_tick != NULL)
                reactor->on_tick(reactor->ctx, budget);
}

void tg_reactor_metrics_take(struct tg_reactor *reactor, uint64_t *turns,
                             uint64_t *events, uint32_t *burst_high_water) {
        if (reactor == NULL)
                return;

        if (turns != NULL)
                *turns = reactor->turns;
        if (events != NULL)
                *events = reactor->events;
        if (burst_high_water != NULL)
                *burst_high_water = reactor->event_burst_high_water;
        reactor->turns = 0;
        reactor->events = 0;
        reactor->event_burst_high_water = 0;
}
