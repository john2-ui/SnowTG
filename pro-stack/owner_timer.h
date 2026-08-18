/**
 * @file owner_timer.h
 * @brief Owner-local timer facade with a replaceable scheduling backend.
 */
#ifndef NETARCH_OWNER_TIMER_H
#define NETARCH_OWNER_TIMER_H

#include <rte_timer.h>
#include <stdbool.h>
#include <stdint.h>

struct owner_timer;
struct owner_timer_engine;

typedef void (*owner_timer_cb)(struct owner_timer *timer, void *arg,
                               uint64_t now_cycles);

/** One embedded timer node. Only its owning lcore may mutate it. */
struct owner_timer {
        owner_timer_cb callback;
        void *callback_arg;
        struct owner_timer_engine *engine;
        struct owner_timer *active_prev;
        struct owner_timer *active_next;
        uint64_t deadline_cycles;
        bool initialized;
        bool armed;
        union {
                struct rte_timer rte;
        } backend;
};

/** Per-worker timer scheduler state. */
struct owner_timer_engine {
        unsigned int lcore_id;
        uint32_t capacity;
        uint32_t active;
        struct owner_timer *active_head;
        bool initialized;
};

/** Initialize the process-wide timer backend once during application startup. */
int owner_timer_global_init(void);
/** Register one engine for @p lcore_id. Initialization may run before launch. */
int owner_timer_engine_init(struct owner_timer_engine *engine,
                            unsigned int lcore_id, uint32_t capacity);
/** Cancel all remaining nodes and unregister the current lcore's engine. */
void owner_timer_engine_fini(struct owner_timer_engine *engine);
/** Return the engine registered for the current lcore, or NULL. */
struct owner_timer_engine *owner_timer_engine_current(void);

/** Initialize an unarmed timer node. */
void owner_timer_init(struct owner_timer *timer, owner_timer_cb callback,
                      void *callback_arg);
/** Arm or rearm at an absolute timer-cycle deadline. */
int owner_timer_arm_at(struct owner_timer *timer, uint64_t deadline_cycles);
/** Arm or rearm after @p delay_ms, saturating cycle conversion on overflow. */
int owner_timer_arm_after_ms(struct owner_timer *timer, uint64_t delay_ms);
/** Idempotently cancel an armed timer. */
int owner_timer_cancel(struct owner_timer *timer);
bool owner_timer_is_armed(const struct owner_timer *timer);

uint64_t owner_timer_now(void);
uint64_t owner_timer_ms_to_cycles(uint64_t milliseconds);
uint64_t owner_timer_cycles_to_ms(uint64_t cycles);
/** Run due callbacks for the current owner. */
int owner_timer_poll(struct owner_timer_engine *engine);

#endif /* NETARCH_OWNER_TIMER_H */
