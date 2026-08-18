/**
 * @file owner_timer.c
 * @brief DPDK rte_timer backend for the owner-local timer facade.
 */
#include "owner_timer.h"

#include "log.h"

#include <errno.h>
#include <limits.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <stddef.h>
#include <string.h>

static struct owner_timer_engine *g_timer_engines[RTE_MAX_LCORE];
static bool g_timer_global_ready;

static int owner_timer_on_owner(const struct owner_timer_engine *engine) {
        return engine != NULL && engine->initialized &&
               rte_lcore_id() == engine->lcore_id;
}

static void owner_timer_active_link(struct owner_timer *timer) {
        struct owner_timer_engine *engine = timer->engine;

        timer->active_prev = NULL;
        timer->active_next = engine->active_head;
        if (engine->active_head != NULL)
                engine->active_head->active_prev = timer;
        engine->active_head = timer;
        engine->active++;
        timer->armed = true;
}

static void owner_timer_active_unlink(struct owner_timer *timer) {
        struct owner_timer_engine *engine = timer->engine;

        if (!timer->armed || engine == NULL)
                return;
        if (timer->active_prev != NULL)
                timer->active_prev->active_next = timer->active_next;
        else
                engine->active_head = timer->active_next;
        if (timer->active_next != NULL)
                timer->active_next->active_prev = timer->active_prev;
        timer->active_prev = NULL;
        timer->active_next = NULL;
        timer->armed = false;
        if (engine->active != 0)
                engine->active--;
}

static void owner_timer_rte_cb(__attribute__((unused)) struct rte_timer *rte,
                               void *arg) {
        struct owner_timer *timer = arg;
        struct owner_timer_engine *engine;
        owner_timer_cb callback;
        void *callback_arg;

        if (timer == NULL || !timer->initialized || !timer->armed)
                return;
        engine = timer->engine;
        if (!owner_timer_on_owner(engine)) {
                LOG_ERROR("owner timer callback on wrong lcore owner=%u caller=%u",
                          engine == NULL ? UINT_MAX : engine->lcore_id,
                          rte_lcore_id());
                return;
        }

        callback = timer->callback;
        callback_arg = timer->callback_arg;
        owner_timer_active_unlink(timer);
        if (callback != NULL)
                callback(timer, callback_arg, owner_timer_now());
}

int owner_timer_global_init(void) {
        if (!g_timer_global_ready) {
                rte_timer_subsystem_init();
                g_timer_global_ready = true;
        }
        return 0;
}

int owner_timer_engine_init(struct owner_timer_engine *engine,
                            unsigned int lcore_id, uint32_t capacity) {
        if (engine == NULL || lcore_id >= RTE_MAX_LCORE || capacity == 0) {
                errno = EINVAL;
                return -1;
        }
        if (g_timer_engines[lcore_id] != NULL) {
                errno = EBUSY;
                return -1;
        }
        memset(engine, 0, sizeof(*engine));
        engine->lcore_id = lcore_id;
        engine->capacity = capacity;
        engine->initialized = true;
        g_timer_engines[lcore_id] = engine;
        return 0;
}

void owner_timer_engine_fini(struct owner_timer_engine *engine) {
        if (engine == NULL || !engine->initialized)
                return;
        if (!owner_timer_on_owner(engine)) {
                LOG_ERROR("reject owner timer fini owner=%u caller=%u",
                          engine->lcore_id, rte_lcore_id());
                return;
        }
        if (engine->active != 0)
                LOG_ERROR("owner timer engine stopped with active timers "
                          "lcore=%u active=%u",
                          engine->lcore_id, engine->active);
        while (engine->active_head != NULL)
                (void)owner_timer_cancel(engine->active_head);
        if (g_timer_engines[engine->lcore_id] == engine)
                g_timer_engines[engine->lcore_id] = NULL;
        memset(engine, 0, sizeof(*engine));
}

struct owner_timer_engine *owner_timer_engine_current(void) {
        unsigned int lcore_id = rte_lcore_id();

        if (lcore_id >= RTE_MAX_LCORE)
                return NULL;
        return g_timer_engines[lcore_id];
}

void owner_timer_init(struct owner_timer *timer, owner_timer_cb callback,
                      void *callback_arg) {
        if (timer == NULL)
                return;
        memset(timer, 0, sizeof(*timer));
        timer->callback = callback;
        timer->callback_arg = callback_arg;
        timer->initialized = true;
        rte_timer_init(&timer->backend.rte);
}

uint64_t owner_timer_now(void) { return rte_get_timer_cycles(); }

uint64_t owner_timer_ms_to_cycles(uint64_t milliseconds) {
        uint64_t hz = rte_get_timer_hz();
        uint64_t seconds = milliseconds / 1000U;
        uint64_t remainder = milliseconds % 1000U;

        if (hz == 0)
                return 0;
        if (seconds > UINT64_MAX / hz)
                return UINT64_MAX;
        uint64_t cycles = seconds * hz;
        uint64_t hz_whole = hz / 1000U;
        uint64_t hz_remainder = hz % 1000U;
        if (remainder != 0 && hz_whole > UINT64_MAX / remainder)
                return UINT64_MAX;
        uint64_t fractional = remainder * hz_whole;
        uint64_t fractional_remainder =
            remainder * hz_remainder / 1000U;
        if (fractional_remainder > UINT64_MAX - fractional)
                return UINT64_MAX;
        fractional += fractional_remainder;
        if (fractional > UINT64_MAX - cycles)
                return UINT64_MAX;
        return cycles + fractional;
}

uint64_t owner_timer_cycles_to_ms(uint64_t cycles) {
        uint64_t hz = rte_get_timer_hz();
        __uint128_t milliseconds;

        if (hz == 0)
                return 0;
        milliseconds = (__uint128_t)cycles * 1000U / hz;
        if (milliseconds > UINT64_MAX)
                return UINT64_MAX;
        return (uint64_t)milliseconds;
}

int owner_timer_arm_at(struct owner_timer *timer, uint64_t deadline_cycles) {
        struct owner_timer_engine *engine;
        uint64_t now;
        uint64_t delay;
        bool newly_armed;

        if (timer == NULL || !timer->initialized || timer->callback == NULL) {
                errno = EINVAL;
                return -1;
        }
        engine = owner_timer_engine_current();
        if (engine == NULL) {
                errno = EPERM;
                return -1;
        }
        if (timer->engine != NULL && timer->engine != engine) {
                errno = EPERM;
                return -1;
        }
        newly_armed = !timer->armed;
        if (newly_armed && engine->active >= engine->capacity) {
                errno = ENOSPC;
                return -1;
        }

        now = owner_timer_now();
        delay = deadline_cycles > now ? deadline_cycles - now : 1U;
        if (rte_timer_reset(&timer->backend.rte, delay, SINGLE,
                            engine->lcore_id, owner_timer_rte_cb, timer) != 0) {
                errno = EBUSY;
                return -1;
        }
        timer->engine = engine;
        timer->deadline_cycles = deadline_cycles;
        if (newly_armed)
                owner_timer_active_link(timer);
        return 0;
}

int owner_timer_arm_after_ms(struct owner_timer *timer, uint64_t delay_ms) {
        uint64_t now = owner_timer_now();
        uint64_t delay = owner_timer_ms_to_cycles(delay_ms);
        uint64_t deadline = delay > UINT64_MAX - now ? UINT64_MAX : now + delay;

        return owner_timer_arm_at(timer, deadline);
}

int owner_timer_cancel(struct owner_timer *timer) {
        if (timer == NULL || !timer->initialized) {
                errno = EINVAL;
                return -1;
        }
        if (!timer->armed)
                return 0;
        if (!owner_timer_on_owner(timer->engine)) {
                errno = EPERM;
                return -1;
        }
        (void)rte_timer_stop(&timer->backend.rte);
        owner_timer_active_unlink(timer);
        return 0;
}

bool owner_timer_is_armed(const struct owner_timer *timer) {
        return timer != NULL && timer->initialized && timer->armed;
}

int owner_timer_poll(struct owner_timer_engine *engine) {
        if (!owner_timer_on_owner(engine)) {
                errno = EPERM;
                return -1;
        }
        rte_timer_manage();
        return 0;
}
