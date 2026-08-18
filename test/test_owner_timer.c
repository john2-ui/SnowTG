#include "../pro-stack/owner_timer.h"

#include <assert.h>
#include <errno.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <stdint.h>
#include <stdlib.h>

struct callback_state {
        unsigned int calls;
        bool rearm;
};

static void count_callback(struct owner_timer *timer, void *arg,
                           __attribute__((unused)) uint64_t now_cycles) {
        struct callback_state *state = arg;

        state->calls++;
        if (state->rearm && state->calls == 1)
                assert(owner_timer_arm_at(timer, owner_timer_now()) == 0);
}

static void free_callback(struct owner_timer *timer,
                          __attribute__((unused)) void *arg,
                          __attribute__((unused)) uint64_t now_cycles) {
        free(timer);
}

int main(int argc, char **argv) {
        struct owner_timer_engine engine;
        struct owner_timer first;
        struct owner_timer second;
        struct callback_state state = {.rearm = true};

        assert(rte_eal_init(argc, argv) >= 0);
        assert(owner_timer_global_init() == 0);
        assert(owner_timer_engine_init(&engine, rte_lcore_id(), 1) == 0);
        owner_timer_init(&first, count_callback, &state);
        owner_timer_init(&second, count_callback, &state);

        assert(owner_timer_arm_at(&first, owner_timer_now()) == 0);
        errno = 0;
        assert(owner_timer_arm_at(&second, UINT64_MAX) == -1);
        assert(errno == ENOSPC);
        assert(owner_timer_poll(&engine) == 0);
        assert(state.calls == 1);
        assert(owner_timer_is_armed(&first));
        assert(engine.active == 1);
        assert(owner_timer_poll(&engine) == 0);
        assert(state.calls == 2);
        assert(!owner_timer_is_armed(&first));
        assert(engine.active == 0);

        assert(owner_timer_cancel(&first) == 0);
        assert(owner_timer_cancel(&first) == 0);
        assert(owner_timer_ms_to_cycles(UINT64_MAX) == UINT64_MAX);
        assert(owner_timer_cycles_to_ms(0) == 0);

        /* Mutating an armed node or polling from a non-owner lcore is denied.
         * Change the test engine's binding temporarily to exercise the check
         * without requiring a second EAL worker or a wall-clock wait. */
        assert(owner_timer_arm_at(&first, UINT64_MAX) == 0);
        unsigned int owner_lcore = engine.lcore_id;
        engine.lcore_id = owner_lcore == 0 ? 1U : 0U;
        errno = 0;
        assert(owner_timer_cancel(&first) == -1);
        assert(errno == EPERM);
        errno = 0;
        assert(owner_timer_poll(&engine) == -1);
        assert(errno == EPERM);
        engine.lcore_id = owner_lcore;
        assert(owner_timer_cancel(&first) == 0);

        struct owner_timer *dynamic = malloc(sizeof(*dynamic));
        assert(dynamic != NULL);
        owner_timer_init(dynamic, free_callback, NULL);
        assert(owner_timer_arm_at(dynamic, owner_timer_now()) == 0);
        assert(owner_timer_poll(&engine) == 0);
        assert(engine.active == 0);

        owner_timer_engine_fini(&engine);
        return 0;
}
