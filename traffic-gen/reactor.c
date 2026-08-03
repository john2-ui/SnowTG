#include "reactor.h"

#include <stddef.h>

/** Initialize callbacks without coupling the reactor to a flow implementation.
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
 * Run scheduling before draining readiness events.
 *
 * The runtime invokes this function on the owner lcore after RX ingress and
 * timer processing, and before transport TX flush.
 */
void tg_reactor_run(void *ctx, unsigned int budget) {
        struct tg_reactor *reactor = ctx;
        struct owner_io_event events[32];
        unsigned int count;

        if (reactor == NULL || budget == 0)
                return;

        if (budget > 32)
                budget = 32;

        /*
         * A future scheduler uses this callback to consume CPS tokens and
         * start at most @p budget new flows.  Every owner_io_* invocation here
         * is valid because this function runs on the socket owner lcore.
         */
        if (reactor->on_tick != NULL)
                reactor->on_tick(reactor->ctx, budget);

        if (reactor->on_event == NULL)
                return;

        count = owner_io_ready_burst(events, budget);
        for (unsigned int i = 0; i < count; i++)
                reactor->on_event(reactor->ctx, &events[i]);
}
