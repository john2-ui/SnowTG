#include "reactor.h"

#include <stddef.h>

/* Store the scheduler callback without tying the reactor to a flow type. */
void tg_reactor_init(struct tg_reactor *reactor, tg_reactor_event_fn on_event,
                     void *ctx) {
        if (reactor == NULL)
                return;
        reactor->on_event = on_event;
        reactor->ctx = ctx;
}

/* Cap each turn to the fixed stack buffer, preserving packet-loop fairness. */
void tg_reactor_run(void *ctx, unsigned int budget) {
        struct tg_reactor *reactor = ctx;
        if (reactor == NULL || reactor->on_event == NULL || budget == 0)
                return;

        struct owner_io_event events[32];
        if (budget > 32)
                budget = 32;

        unsigned int count = owner_io_ready_burst(events, budget);
        for (unsigned int i = 0; i < count; i++)
                reactor->on_event(reactor->ctx, &events[i]);
}
