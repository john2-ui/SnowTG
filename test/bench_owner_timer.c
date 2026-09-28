/* Run each backend in a separate, CPU-pinned process. Not a correctness gate. */
#include "../pro-stack/owner_timer.h"
#include <assert.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t callbacks;
static void expired(struct owner_timer *timer, void *arg, uint64_t now) {
        (void)arg;
        assert(now >= timer->deadline_cycles);
        callbacks++;
}
static void row(unsigned n, const char *operation, uint64_t ops, uint64_t start) {
        uint64_t cycles = owner_timer_now() - start;
        printf("%u,%s,%" PRIu64 ",%" PRIu64 "\n", n, operation, ops, cycles);
}
int main(int argc, char **argv) {
        assert(rte_eal_init(argc, argv) >= 0);
        assert(owner_timer_global_init() == 0);
        puts("capacity,operation,operations,cycles");
        unsigned counts[] = {512, 4096, 65536};
        for (unsigned c = 0; c < 3; c++) {
                unsigned n = counts[c];
                struct owner_timer_engine engine;
                struct owner_timer *timers = calloc(n, sizeof(*timers));
                assert(timers != NULL);
                assert(owner_timer_engine_init(&engine, rte_lcore_id(), n) == 0);
                for (unsigned i = 0; i < n; i++)
                        owner_timer_init(&timers[i], expired, NULL);
                for (unsigned repeat = 0; repeat < 10; repeat++) {
                        uint64_t base = owner_timer_now();
                        uint64_t deadline = base + owner_timer_ms_to_cycles(60000);
                        for (unsigned i = 0; i < n; i++)
                                assert(owner_timer_arm_at(&timers[i], deadline + i) == 0);
                        row(n, "arm", n, base);
                        base = owner_timer_now();
                        for (unsigned i = 0; i < n; i++)
                                assert(owner_timer_arm_at(&timers[i], deadline + n - i) == 0);
                        row(n, "rearm", n, base);
                        base = owner_timer_now();
                        for (unsigned i = 0; i < 100000; i++)
                                assert(owner_timer_poll(&engine) == 0);
                        row(n, "future_poll", 100000, base);
                        base = owner_timer_now();
                        for (unsigned i = 0; i < n; i++)
                                assert(owner_timer_cancel(&timers[i]) == 0);
                        row(n, "cancel", n, base);
                        for (unsigned i = 0; i < n; i++)
                                assert(owner_timer_arm_at(&timers[i], owner_timer_now()) == 0);
                        uint64_t before = callbacks;
                        base = owner_timer_now();
                        assert(owner_timer_poll(&engine) == 0);
                        row(n, "expire", n, base);
                        assert(callbacks - before == n && engine.active == 0);
                        base = owner_timer_now();
                        for (unsigned i = 0; i < 100000; i++)
                                assert(owner_timer_poll(&engine) == 0);
                        row(n, "empty_poll", 100000, base);
                        base = owner_timer_now();
                        for (unsigned i = 0; i < n; i++)
                                assert(owner_timer_arm_after_ms(&timers[i], i % 2 ? 1000 : 3600000) == 0);
                        assert(owner_timer_poll(&engine) == 0);
                        for (unsigned i = 0; i < n; i++)
                                assert(owner_timer_cancel(&timers[i]) == 0);
                        row(n, "mixed_arm_poll_cancel", n, base);
                }
                owner_timer_engine_fini(&engine);
                free(timers);
        }
        return 0;
}
