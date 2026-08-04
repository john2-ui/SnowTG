#include "reactor.h"

#include <stddef.h>

void tg_reactor_init(struct tg_reactor *reactor, tg_reactor_tick_fn on_tick,
                     tg_reactor_event_fn on_event, void *ctx) {
        if (reactor == NULL)
                return;

        reactor->on_tick = on_tick;
        reactor->on_event = on_event;
        reactor->ctx = ctx;
}

void tg_reactor_run(void *ctx, unsigned int budget) {
        struct tg_reactor *reactor = ctx;
        struct owner_io_event events[32];
        unsigned int count;

        if (reactor == NULL || budget == 0)
                return;
        if (budget > 32)
                budget = 32;

        if (reactor->on_tick != NULL)
                reactor->on_tick(reactor->ctx, budget);
        if (reactor->on_event == NULL)
                return;

        count = owner_io_ready_burst(events, budget);
        for (unsigned int i = 0; i < count; i++)
                reactor->on_event(reactor->ctx, &events[i]);
}
