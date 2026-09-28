/* Deterministic clock seam: not linked into the production archive. */
#define OWNER_TIMER_WHEEL 1
#define OWNER_TIMER_TESTING 1
#include "../pro-stack/owner_timer.c"
#include <assert.h>
#include <rte_eal.h>
#include <sys/mman.h>
#include <unistd.h>

struct node {
        struct owner_timer timer;
        unsigned calls;
        bool pending;
};
static void count(struct owner_timer *timer, void *arg, uint64_t now) {
        struct node *node = arg;
        assert(!owner_timer_is_armed(timer));
        assert(now >= timer->deadline_cycles);
        node->calls++;
        node->pending = false;
}
static void arm(struct node *node, uint64_t deadline) {
        assert(owner_timer_arm_at(&node->timer, deadline) == 0);
        node->pending = true;
}
static void poll_at(struct owner_timer_engine *engine, uint64_t now) {
        owner_timer_test_now = now;
        assert(owner_timer_poll(engine) == 0);
}
static void destroy_other(struct owner_timer *timer, void *arg, uint64_t now) {
        struct owner_timer *other = arg;
        (void)now;
        assert(owner_timer_cancel(other) == 0);
        assert(munmap(other, (size_t)sysconf(_SC_PAGESIZE)) == 0);
        assert(owner_timer_arm_at(timer, owner_timer_now()) == 0);
        /* Recursive polling must not invalidate the outer ready queue. */
        assert(owner_timer_poll(timer->engine) == 0);
}

int main(int argc, char **argv) {
        assert(rte_eal_init(argc, argv) >= 0);
        struct owner_timer_engine engine;
        struct node nodes[1024] = {0};
        assert(owner_timer_engine_init(&engine, rte_lcore_id(), 1024) == 0);
        for (unsigned i = 0; i < 1024; i++)
                owner_timer_init(&nodes[i].timer, count, &nodes[i]);

        arm(&nodes[0], 1001);
        poll_at(&engine, 1001);
        assert(nodes[0].calls == 0);
        poll_at(&engine, 1999);
        assert(nodes[0].calls == 0);
        poll_at(&engine, 2000);
        assert(nodes[0].calls == 1);

        /* A non-integral cycles-per-ms clock rounds up without firing early,
         * including the last representable absolute cycle deadline. */
        owner_timer_engine_fini(&engine);
        owner_timer_test_hz = 1000003;
        owner_timer_test_now = 0;
        assert(owner_timer_engine_init(&engine, rte_lcore_id(), 1024) == 0);
        unsigned before = nodes[0].calls;
        arm(&nodes[0], 2001);
        poll_at(&engine, 3000);
        assert(nodes[0].calls == before);
        poll_at(&engine, 3001);
        assert(nodes[0].calls == before + 1);
        arm(&nodes[0], UINT64_MAX);
        poll_at(&engine, UINT64_MAX - 1);
        assert(nodes[0].calls == before + 1);
        poll_at(&engine, UINT64_MAX);
        assert(nodes[0].calls == before + 2);

        /* Move across every radix level, including the highest byte, with
         * gaps too large for a per-tick catch-up loop to finish. */
        owner_timer_engine_fini(&engine);
        owner_timer_test_now = 0;
        owner_timer_test_hz = 1000;
        assert(owner_timer_engine_init(&engine, rte_lcore_id(), 1024) == 0);
        for (unsigned level = 1; level < 8; level++) {
                uint64_t boundary = UINT64_C(1) << (level * 8);
                arm(&nodes[level], boundary + 17);
                poll_at(&engine, boundary - 1);
                assert(nodes[level].calls == 0);
                poll_at(&engine, boundary);
                assert(nodes[level].calls == 0);
                poll_at(&engine, boundary + 17);
                assert(nodes[level].calls == 1);
        }
        arm(&nodes[8], UINT64_MAX);
        poll_at(&engine, UINT64_MAX - 1);
        assert(nodes[8].calls == 0);
        poll_at(&engine, UINT64_MAX);
        assert(nodes[8].calls == 1 && engine.active == 0);
        owner_timer_engine_fini(&engine);

        owner_timer_test_now = 0;
        assert(owner_timer_engine_init(&engine, rte_lcore_id(), 1024) == 0);
        /* Compare mixed insert/rearm/cancel and irregular polls to an oracle. */
        uint32_t random = 19;
        for (unsigned step = 0; step < 30000; step++) {
                random = random * 1664525U + 1013904223U;
                struct node *node = &nodes[random % 1024];
                if (random & 1024) {
                        arm(node, owner_timer_now() + 1 + (random >> 12));
                } else {
                        assert(owner_timer_cancel(&node->timer) == 0);
                        node->pending = false;
                }
                if (step % 31 == 0) {
                        uint64_t now = owner_timer_now() + 7919;
                        unsigned expected[1024];
                        for (unsigned i = 0; i < 1024; i++)
                                expected[i] = nodes[i].calls +
                                    (nodes[i].pending && nodes[i].timer.deadline_cycles <= now);
                        poll_at(&engine, now);
                        for (unsigned i = 0; i < 1024; i++)
                                assert(nodes[i].calls == expected[i]);
                }
        }
        poll_at(&engine, UINT64_MAX);
        assert(engine.active == 0);
        owner_timer_engine_fini(&engine);

        owner_timer_test_now = 0;
        assert(owner_timer_engine_init(&engine, rte_lcore_id(), 2) == 0);
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        struct owner_timer *victim = mmap(NULL, page, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(victim != MAP_FAILED);
        owner_timer_init(victim, count, &nodes[0]);
        owner_timer_init(&nodes[0].timer, destroy_other, victim);
        /* Immediate queue reverses once into ready: first arm executes first. */
        arm(&nodes[0], 0);
        assert(owner_timer_arm_at(victim, 0) == 0);
        poll_at(&engine, 0);
        assert(engine.active == 1);
        assert(owner_timer_cancel(&nodes[0].timer) == 0);
        owner_timer_engine_fini(&engine);
        return 0;
}
