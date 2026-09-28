/**
 * @file owner_timer.c
 * @brief Selectable rte_timer / hierarchical wheel owner-local scheduler.
 */
#include "owner_timer.h"

#include "log.h"

#include <errno.h>
#include <limits.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#ifndef OWNER_TIMER_WHEEL
#define OWNER_TIMER_WHEEL 0
#endif

static struct owner_timer_engine *g_timer_engines[RTE_MAX_LCORE];
#if !OWNER_TIMER_WHEEL
static bool g_timer_global_ready;
#endif

#ifdef OWNER_TIMER_TESTING
/* Linked only into deterministic tests, never into the production archive. */
uint64_t owner_timer_test_now;
uint64_t owner_timer_test_hz = 1000000;
#define TIMER_HZ() owner_timer_test_hz
#else
#define TIMER_HZ() rte_get_timer_hz()
#endif

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

#if OWNER_TIMER_WHEEL
#define WHEEL_LEVELS 8U
#define WHEEL_SLOTS 256U
#define WHEEL_BUCKETS (WHEEL_LEVELS * WHEEL_SLOTS)

struct owner_timer_wheel {
        struct owner_timer *buckets[WHEEL_BUCKETS];
        uint64_t occupied[WHEEL_LEVELS][4];
        struct owner_timer *immediate;
        uint64_t cursor, next_tick, next_cycles;
        bool polling;
};

static uint64_t wheel_tick(uint64_t cycles, bool round_up) {
        uint64_t hz = TIMER_HZ();
        __uint128_t ticks;

        if (hz == 0)
                return 0;
        ticks = (__uint128_t)cycles * 1000U;
        if (round_up)
                ticks += hz - 1;
        ticks /= hz;
        return ticks > UINT64_MAX ? UINT64_MAX : (uint64_t)ticks;
}

/* head may be a bucket, the immediate queue, or this poll's ready queue.
 * Keeping its address permits cancellation of another callback's ready node. */
static void wheel_link(struct owner_timer *timer, struct owner_timer **head,
                       unsigned bucket) {
        timer->backend.wheel.prev = NULL;
        timer->backend.wheel.next = *head;
        timer->backend.wheel.head = head;
        timer->backend.wheel.bucket = bucket;
        if (*head != NULL)
                (*head)->backend.wheel.prev = timer;
        *head = timer;
}

static void wheel_unlink(struct owner_timer *timer) {
        struct owner_timer *prev = timer->backend.wheel.prev;
        struct owner_timer *next = timer->backend.wheel.next;
        struct owner_timer **head = timer->backend.wheel.head;
        unsigned bucket = timer->backend.wheel.bucket;

        if (head == NULL)
                return;
        if (prev != NULL)
                prev->backend.wheel.next = next;
        else
                *head = next;
        if (next != NULL)
                next->backend.wheel.prev = prev;
        if (*head == NULL && bucket < WHEEL_BUCKETS)
                timer->engine->wheel->occupied[bucket / 256][bucket % 256 / 64]
                    &= ~(UINT64_C(1) << (bucket % 64));
        timer->backend.wheel.head = NULL;
        timer->backend.wheel.prev = timer->backend.wheel.next = NULL;
}

static uint64_t wheel_boundary(uint64_t cursor, unsigned level,
                                unsigned slot) {
        unsigned shift = level * 8;
        uint64_t prefix = level == 7 ? 0 : cursor &
            ~(UINT64_MAX >> (64 - (shift + 8)));
        return prefix | ((uint64_t)slot << shift);
}

static void wheel_cache_next(struct owner_timer_wheel *wheel) {
        __uint128_t cycles = ((__uint128_t)wheel->next_tick * TIMER_HZ() + 999) / 1000;
        wheel->next_cycles = cycles > UINT64_MAX ? UINT64_MAX : (uint64_t)cycles;
}

static void wheel_schedule(struct owner_timer *timer) {
        struct owner_timer_wheel *wheel = timer->engine->wheel;
        uint64_t tick = timer->backend.wheel.tick;
        unsigned level, slot, bucket;

        if (tick <= wheel->cursor) {
                wheel_link(timer, &wheel->immediate, WHEEL_BUCKETS);
                return;
        }
        /* The highest differing byte identifies the next cascade boundary.
         * Within a level all occupied slots are ahead of the cursor. */
        level = (63U - (unsigned)__builtin_clzll(tick ^ wheel->cursor)) / 8U;
        slot = (tick >> (8U * level)) & 255U;
        bucket = level * 256U + slot;
        wheel_link(timer, &wheel->buckets[bucket], bucket);
        wheel->occupied[level][slot / 64] |= UINT64_C(1) << (slot % 64);
        uint64_t boundary = wheel_boundary(wheel->cursor, level, slot);
        if (boundary < wheel->next_tick) {
                wheel->next_tick = boundary;
                wheel_cache_next(wheel);
        }
}

static unsigned wheel_next(struct owner_timer_wheel *wheel) {
        unsigned selected = WHEEL_BUCKETS;
        wheel->next_tick = UINT64_MAX;
        for (unsigned level = 0; level < WHEEL_LEVELS; level++) {
                for (unsigned word = 0; word < 4; word++) {
                        uint64_t bits = wheel->occupied[level][word];
                        if (bits == 0)
                                continue;
                        unsigned slot = word * 64U + __builtin_ctzll(bits);
                        uint64_t boundary = wheel_boundary(wheel->cursor,
                                                            level, slot);
                        if (selected == WHEEL_BUCKETS ||
                            boundary < wheel->next_tick) {
                                wheel->next_tick = boundary;
                                selected = level * 256U + slot;
                        }
                        break;
                }
        }
        wheel_cache_next(wheel);
        return selected;
}

static void wheel_poll(struct owner_timer_engine *engine) {
        struct owner_timer_wheel *wheel = engine->wheel;
        struct owner_timer *ready = NULL, *timer;
        uint64_t now, target;

        if (wheel->polling || engine->active == 0)
                return;
        now = owner_timer_now();
        if (wheel->immediate == NULL && now < wheel->next_cycles)
                return;
        target = wheel_tick(now, now == UINT64_MAX);
        wheel->polling = true;
        /* Snapshot due work before invoking callbacks. Immediate callback
         * rearms stay on the engine's queue until the next poll. */
        while ((timer = wheel->immediate) != NULL) {
                wheel_unlink(timer);
                wheel_link(timer, &ready, WHEEL_BUCKETS);
        }
        while (wheel->next_tick <= target) {
                unsigned bucket = wheel_next(wheel);
                if (bucket == WHEEL_BUCKETS || wheel->next_tick > target)
                        break;
                wheel->cursor = wheel->next_tick;
                while ((timer = wheel->buckets[bucket]) != NULL) {
                        wheel_unlink(timer);
                        if (timer->backend.wheel.tick <= target &&
                            timer->deadline_cycles <= now)
                                wheel_link(timer, &ready, WHEEL_BUCKETS);
                        else
                                wheel_schedule(timer);
                }
                wheel_next(wheel);
        }
        if (target > wheel->cursor)
                wheel->cursor = target;
        while ((timer = ready) != NULL) {
                owner_timer_cb callback = timer->callback;
                void *arg = timer->callback_arg;
                wheel_unlink(timer);
                owner_timer_active_unlink(timer);
                callback(timer, arg, now);
        }
        wheel->polling = false;
}
#endif

#if !OWNER_TIMER_WHEEL
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
        /* The callback may free its enclosing socket or hand its storage to
         * another owner. Stop the RUNNING backend before that can happen:
         * rte_timer_manage() otherwise touches it after the callback returns. */
        if (rte_timer_stop(&timer->backend.rte) != 0) {
                LOG_ERROR("cannot detach running owner timer lcore=%u",
                          rte_lcore_id());
                return;
        }
        owner_timer_active_unlink(timer);
        if (callback != NULL)
                callback(timer, callback_arg, owner_timer_now());
}

#endif

int owner_timer_global_init(void) {
#if !OWNER_TIMER_WHEEL
        if (!g_timer_global_ready) {
                rte_timer_subsystem_init();
                g_timer_global_ready = true;
        }
#endif
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
#if OWNER_TIMER_WHEEL
        engine->wheel = calloc(1, sizeof(*engine->wheel));
        if (engine->wheel == NULL) {
                errno = ENOMEM;
                return -1;
        }
        engine->wheel->cursor = wheel_tick(owner_timer_now(), false);
        engine->wheel->next_tick = UINT64_MAX;
        engine->wheel->next_cycles = UINT64_MAX;
#endif
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
        free(engine->wheel);
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
#if !OWNER_TIMER_WHEEL
        rte_timer_init(&timer->backend.rte);
#endif
}

uint64_t owner_timer_now(void) {
#ifdef OWNER_TIMER_TESTING
        return owner_timer_test_now;
#else
        return rte_get_timer_cycles();
#endif
}

uint64_t owner_timer_ms_to_cycles(uint64_t milliseconds) {
        uint64_t hz = TIMER_HZ();
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
        uint64_t hz = TIMER_HZ();
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
#if !OWNER_TIMER_WHEEL
        uint64_t now;
        uint64_t delay;
#endif
        bool newly_armed;

        if (timer == NULL || !timer->initialized || timer->callback == NULL) {
                errno = EINVAL;
                return -1;
        }
        engine = owner_timer_engine_current();
        if (!owner_timer_on_owner(engine)) {
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

#if OWNER_TIMER_WHEEL
        if (timer->armed)
                wheel_unlink(timer);
#else
        now = owner_timer_now();
        delay = deadline_cycles > now ? deadline_cycles - now : 1U;
        if (rte_timer_reset(&timer->backend.rte, delay, SINGLE,
                            engine->lcore_id, owner_timer_rte_cb, timer) != 0) {
                errno = EBUSY;
                return -1;
        }
#endif
        timer->engine = engine;
        timer->deadline_cycles = deadline_cycles;
        if (newly_armed)
                owner_timer_active_link(timer);
#if OWNER_TIMER_WHEEL
        timer->backend.wheel.tick = wheel_tick(deadline_cycles, true);
        if (deadline_cycles <= owner_timer_now())
                wheel_link(timer, &engine->wheel->immediate, WHEEL_BUCKETS);
        else
                wheel_schedule(timer);
#endif
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
#if OWNER_TIMER_WHEEL
        wheel_unlink(timer);
#else
        (void)rte_timer_stop(&timer->backend.rte);
#endif
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
#if OWNER_TIMER_WHEEL
        wheel_poll(engine);
#else
        rte_timer_manage();
#endif
        return 0;
}
