#ifndef TRAFFIC_GEN_REACTOR_H
#define TRAFFIC_GEN_REACTOR_H

#include "../../pro-stack/owner_io.h"

typedef void (*tg_reactor_tick_fn)(void *ctx, unsigned int budget);
typedef void (*tg_reactor_event_fn)(void *ctx,
                                    const struct owner_io_event *event);

struct tg_reactor {
        tg_reactor_event_fn on_event;
        tg_reactor_tick_fn on_tick;
        void *ctx;
};

void tg_reactor_init(struct tg_reactor *reactor, tg_reactor_tick_fn on_tick,
                     tg_reactor_event_fn on_event, void *ctx);
void tg_reactor_run(void *ctx, unsigned int budget);

#endif /* TRAFFIC_GEN_REACTOR_H */
