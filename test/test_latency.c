/* Check full-range bucket bounds, merge-before-quantile semantics and
 * transaction attribution without DPDK clocks or a responding peer. */
#include "../traffic-gen/core/latency.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void test_histogram(void) {
        struct tg_histogram *a = calloc(1, sizeof(*a));
        struct tg_histogram *b = calloc(1, sizeof(*b));
        assert(a && b);
        assert(tg_histogram_quantile(a, 990) == 0);
        /* Unequal populations: averaging the two workers' P99 is incorrect. */
        for (unsigned int i = 0; i < 999; i++)
                tg_histogram_record(a, 10);
        tg_histogram_record(b, 1000000);
        tg_histogram_merge(a, b);
        assert(a->count == 1000 && a->max_us == 1000000);
        assert(tg_histogram_quantile(a, 990) == 10);
        assert(tg_histogram_quantile(a, 999) == 10);
        assert(tg_histogram_quantile(a, 1000) == 1000000);
        for (unsigned int bit = 0; bit < 64; bit++) {
                uint64_t value = UINT64_C(1) << bit;
                memset(b, 0, sizeof(*b));
                tg_histogram_record(b, value - 1);
                tg_histogram_record(b, value);
                tg_histogram_record(b, UINT64_MAX);
                uint64_t q = tg_histogram_quantile(b, 500);
                assert(q >= value && q - value <= value / 16);
                assert(tg_histogram_quantile(b, 1000) == UINT64_MAX);
        }
        free(a);
        free(b);
}

static void test_timing_and_attribution(void) {
        struct tg_plan plan = {.phase_count = 2, .class_count = 2};
        struct tg_latency latency;
        struct tg_flow flow = {.start_cycles = 200,
                               .connected_cycles = 250,
                               .first_rx_cycles = 300};
        assert(tg_latency_init(&latency, &plan, 1000000) == 0);
        tg_latency_on_admitted(&latency, &flow, 0, 1, 100);
        /* Completion after a phase boundary stays in its planned phase/class.
         */
        tg_latency_on_finished(&latency, &flow, TG_FLOW_RESULT_SUCCESS, 400);
        const struct tg_latency_group *g = &latency.groups[1];
        assert(g->admitted == 1 && g->success == 1 && g->failed == 0);
        assert(g->hist[TG_LAT_SCHEDULE].max_us == 100);
        assert(g->hist[TG_LAT_CONNECT].max_us == 50);
        assert(g->hist[TG_LAT_FIRST_RX].max_us == 100);
        assert(g->hist[TG_LAT_COMPLETE].max_us == 200);
        assert(g->hist[TG_LAT_SCHEDULED_COMPLETE].max_us == 300);
        /* Reuse resets connect timing; no fake zero-latency connect sample. */
        flow.start_cycles = 600;
        flow.connected_cycles = 0;
        flow.first_rx_cycles = 0;
        tg_latency_on_admitted(&latency, &flow, 1, 0, 500);
        tg_latency_on_finished(&latency, &flow, TG_FLOW_RESULT_IO_FAILURE,
                               5600);
        g = &latency.groups[2];
        assert(g->hist[TG_LAT_CONNECT].count == 0 &&
               g->hist[TG_LAT_FIRST_RX].count == 0);
        assert(g->failed == 1 &&
               g->hist[TG_LAT_COMPLETE_FAILURE].max_us == 5000);
        assert(g->hist[TG_LAT_COMPLETE_SUCCESS].count == 0);
        tg_latency_on_start_failed(&latency, 1, 0);
        assert(g->start_failed == 1 && g->admitted == 1);
        tg_latency_on_drained(&latency, 10000, 12000, true);
        assert(latency.drained && latency.drain_us == 2000);
        tg_latency_on_drained(&latency, 10000, 12000, false);
        assert(!latency.drained);
        tg_latency_fini(&latency);
}

int main(void) {
        test_histogram();
        test_timing_and_attribution();
        return 0;
}
