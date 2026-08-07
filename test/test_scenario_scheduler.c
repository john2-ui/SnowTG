#include "../traffic-gen/core/scenario.h"
#include "../traffic-gen/core/scheduler.h"
#include "../traffic-gen/core/stats.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(condition)                                                 \
        do {                                                                   \
                if (!(condition)) {                                            \
                        fprintf(stderr, "%s:%d assertion failed: %s\n",        \
                                __FILE__, __LINE__, #condition);               \
                        return -1;                                             \
                }                                                              \
        } while (0)

struct start_recorder {
        char selected[8];
        size_t count;
        int result;
};

static int record_start(void *ctx, const struct tg_class_plan *class_plan) {
        struct start_recorder *recorder = ctx;

        recorder->selected[recorder->count++] = class_plan->name[0];
        return recorder->result;
}

static void make_scheduler_plan(struct tg_plan *plan) {
        memset(plan, 0, sizeof(*plan));
        plan->duration_sec = 2;
        plan->max_concurrency = 3;
        plan->target_cps = 10;
        plan->total_weight = 3;
        plan->class_count = 2;
        plan->classes[0].weight = 2;
        plan->classes[1].weight = 1;
        strcpy(plan->classes[0].name, "A");
        strcpy(plan->classes[1].name, "B");
}

static int test_plan_load_and_validation(void) {
        struct tg_plan plan;

        ASSERT_TRUE(
            tg_plan_load_file(
                &plan, "../traffic-gen/scenarios/bootstrap_http.json") == 0);
        ASSERT_TRUE(strcmp(plan.name, "bootstrap-http") == 0);
        ASSERT_TRUE(plan.class_count == 1);
        ASSERT_TRUE(plan.total_weight == 1);
        ASSERT_TRUE(plan.classes[0].proto == &tg_http_proto_ops);
        ASSERT_TRUE(strcmp(plan.classes[0].http_config.method, "GET") == 0);
        ASSERT_TRUE(strcmp(plan.classes[0].http_config.path, "/") == 0);
        ASSERT_TRUE(plan.classes[0].request_template_len != 0);
        ASSERT_TRUE(memcmp(plan.classes[0].request_template,
                           "GET / HTTP/1.0\r\nConnection: close\r\n\r\n",
                           plan.classes[0].request_template_len) == 0);
        tg_plan_fini(&plan);

        errno = 0;
        ASSERT_TRUE(
            tg_plan_load_file(&plan, "fixtures/invalid_scenario.json") == -1);
        ASSERT_TRUE(errno == EINVAL);
        return 0;
}

static int test_plan_partition(void) {
        struct tg_plan source;
        struct tg_plan shards[3];
        uint32_t total_cps = 0;
        uint32_t total_concurrency = 0;

        make_scheduler_plan(&source);
        for (unsigned int index = 0; index < 3; index++) {
                ASSERT_TRUE(
                    tg_plan_partition(&shards[index], &source, index, 3) == 0);
                total_cps += shards[index].target_cps;
                total_concurrency += shards[index].max_concurrency;
                ASSERT_TRUE(shards[index].classes[0].http_config.method ==
                            shards[index].classes[0].http_method);
                ASSERT_TRUE(shards[index].classes[0].http_config.path ==
                            shards[index].classes[0].http_path);
        }
        ASSERT_TRUE(shards[0].target_cps == 4);
        ASSERT_TRUE(shards[1].target_cps == 3);
        ASSERT_TRUE(shards[2].target_cps == 3);
        ASSERT_TRUE(total_cps == source.target_cps);
        ASSERT_TRUE(total_concurrency == source.max_concurrency);
        errno = 0;
        ASSERT_TRUE(tg_plan_partition(&shards[0], &source, 3, 3) == -1);
        ASSERT_TRUE(errno == EINVAL);
        return 0;
}

static int test_weight_token_and_concurrency_bounds(void) {
        struct tg_plan plan;
        struct tg_scheduler scheduler;
        struct start_recorder recorder = {0};

        make_scheduler_plan(&plan);
        ASSERT_TRUE(tg_scheduler_init(&scheduler, &plan, 10) == 0);
        ASSERT_TRUE(
            tg_scheduler_tick(&scheduler, 0, 3, record_start, &recorder) == 0);
        ASSERT_TRUE(
            tg_scheduler_tick(&scheduler, 10, 3, record_start, &recorder) == 3);
        ASSERT_TRUE(recorder.count == 3);
        ASSERT_TRUE(memcmp(recorder.selected, "AAB", 3) == 0);
        ASSERT_TRUE(scheduler.active == 3);

        tg_scheduler_on_flow_finished(&scheduler);
        ASSERT_TRUE(
            tg_scheduler_tick(&scheduler, 11, 3, record_start, &recorder) == 1);
        ASSERT_TRUE(recorder.selected[3] == 'A');
        ASSERT_TRUE(scheduler.active == 3);

        tg_scheduler_set_resource_available(&scheduler, false);
        ASSERT_TRUE(
            tg_scheduler_tick(&scheduler, 12, 3, record_start, &recorder) == 0);
        ASSERT_TRUE(scheduler.resource_paused);
        ASSERT_TRUE(scheduler.resource_pauses == 1);
        tg_scheduler_set_resource_available(&scheduler, true);
        tg_scheduler_on_flow_finished(&scheduler);
        ASSERT_TRUE(
            tg_scheduler_tick(&scheduler, 13, 3, record_start, &recorder) == 1);
        return 0;
}

static int test_failed_start_and_duration_stop(void) {
        struct tg_plan plan;
        struct tg_scheduler scheduler;
        struct start_recorder recorder = {.result = -1};

        make_scheduler_plan(&plan);
        ASSERT_TRUE(tg_scheduler_init(&scheduler, &plan, 10) == 0);
        ASSERT_TRUE(
            tg_scheduler_tick(&scheduler, 0, 2, record_start, &recorder) == 0);
        ASSERT_TRUE(
            tg_scheduler_tick(&scheduler, 10, 2, record_start, &recorder) == 2);
        ASSERT_TRUE(scheduler.active == 0);

        ASSERT_TRUE(tg_scheduler_init(&scheduler, &plan, 10) == 0);
        ASSERT_TRUE(
            tg_scheduler_tick(&scheduler, 0, 1, record_start, &recorder) == 0);
        ASSERT_TRUE(
            tg_scheduler_tick(&scheduler, 20, 1, record_start, &recorder) == 0);
        ASSERT_TRUE(tg_scheduler_is_stopped(&scheduler));
        return 0;
}

static int test_stats(void) {
        struct tg_stats stats;
        struct tg_flow flow;

        memset(&flow, 0, sizeof(flow));
        flow.txn.request_offset = 12;
        flow.txn.response_bytes = 34;
        flow.txn.proto = &tg_http_proto_ops;
        tg_stats_init(&stats);
        tg_stats_on_admitted(&stats);
        tg_stats_on_flow_finished(&stats, &flow, TG_FLOW_RESULT_SUCCESS);
        ASSERT_TRUE(stats.concurrency == 0);
        ASSERT_TRUE(stats.txns_started == 1);
        ASSERT_TRUE(stats.txns_success == 1);
        ASSERT_TRUE(stats.bytes_tx == 12);
        ASSERT_TRUE(stats.bytes_rx == 34);
        ASSERT_TRUE(stats.http_rps_total == 1);
        tg_stats_on_resource_deferred(&stats);
        ASSERT_TRUE(stats.starts_deferred_resource == 1);
        return 0;
}

int main(void) {
        ASSERT_TRUE(test_plan_load_and_validation() == 0);
        ASSERT_TRUE(test_plan_partition() == 0);
        ASSERT_TRUE(test_weight_token_and_concurrency_bounds() == 0);
        ASSERT_TRUE(test_failed_start_and_duration_stop() == 0);
        ASSERT_TRUE(test_stats() == 0);
        return EXIT_SUCCESS;
}
