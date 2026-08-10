#include "../traffic-gen/core/scenario.h"
#include "../traffic-gen/core/scheduler.h"
#include "../traffic-gen/core/stats.h"
#include "../traffic-gen/proto/dns/dns_client.h"
#include "../traffic-gen/proto/http/http_client.h"

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

enum { PARTITION_FAILURE_CLASS_COUNT = 3 };

static void *partition_source_configs[PARTITION_FAILURE_CLASS_COUNT];
static unsigned int partition_clone_calls;
static unsigned int partition_config_free_calls;
static unsigned int partition_fail_at;
static unsigned int partition_source_free_during_failure;
static int partition_cleanup_in_progress;

static int partition_test_config_clone(const void *source, void **destination) {
        int *copy;

        if (source == NULL || destination == NULL) {
                errno = EINVAL;
                return -1;
        }
        partition_clone_calls++;
        copy = malloc(sizeof(*copy));
        if (copy == NULL)
                return -1;
        *copy = *(const int *)source;
        *destination = copy;
        if (partition_clone_calls == partition_fail_at) {
                errno = EIO;
                return -1;
        }
        return 0;
}

static void partition_test_config_free(void *config) {
        for (unsigned int index = 0; index < PARTITION_FAILURE_CLASS_COUNT;
             index++) {
                if (config != partition_source_configs[index])
                        continue;
                if (partition_cleanup_in_progress) {
                        partition_source_free_during_failure++;
                        return;
                }
                break;
        }
        partition_config_free_calls++;
        free(config);
}

static const struct tg_proto_ops partition_test_proto_ops = {
    .name = "partition-failure-test",
    .config_clone = partition_test_config_clone,
    .config_free = partition_test_config_free,
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
        const struct tg_http_config *http_config;

        ASSERT_TRUE(
            tg_plan_load_file(
                &plan, "../traffic-gen/scenarios/bootstrap_http.json") == 0);
        ASSERT_TRUE(strcmp(plan.name, "bootstrap-http") == 0);
        ASSERT_TRUE(plan.class_count == 1);
        ASSERT_TRUE(plan.total_weight == 1);
        ASSERT_TRUE(plan.classes[0].proto == &tg_http_proto_ops);
        http_config = plan.classes[0].proto_config;
        ASSERT_TRUE(http_config != NULL);
        ASSERT_TRUE(strcmp(http_config->method, "GET") == 0);
        ASSERT_TRUE(strcmp(http_config->path, "/") == 0);
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

static int test_mixed_protocol_plan_and_partition(void) {
        struct tg_plan source;
        struct tg_plan shards[2] = {0};
        const struct tg_http_config *source_http;
        const struct tg_dns_config *source_dns;
        uint32_t total_cps = 0;
        uint32_t total_concurrency = 0;

        ASSERT_TRUE(
            tg_plan_load_file(
                &source, "../traffic-gen/scenarios/test/mix-http-dns.json") ==
            0);
        ASSERT_TRUE(source.class_count == 2);
        ASSERT_TRUE(source.total_weight == 10);
        ASSERT_TRUE(source.classes[0].transport == TG_TRANSPORT_TCP);
        ASSERT_TRUE(source.classes[0].proto == &tg_http_proto_ops);
        ASSERT_TRUE(source.classes[1].transport == TG_TRANSPORT_UDP);
        ASSERT_TRUE(source.classes[1].proto == &tg_dns_proto_ops);
        source_http = source.classes[0].proto_config;
        source_dns = source.classes[1].proto_config;
        ASSERT_TRUE(source_http != NULL);
        ASSERT_TRUE(source_dns != NULL);
        ASSERT_TRUE(strcmp(source_http->path, "/") == 0);
        ASSERT_TRUE(strcmp(source_dns->qname, "www.example.local") == 0);
        ASSERT_TRUE(source_dns->qtype == TG_DNS_QTYPE_A);
        ASSERT_TRUE(source.classes[1].request_template_len > 16);

        source.target_cps = 10;
        source.max_concurrency = 4;
        for (unsigned int index = 0; index < 2; index++) {
                const struct tg_http_config *shard_http;
                const struct tg_dns_config *shard_dns;

                ASSERT_TRUE(
                    tg_plan_partition(&shards[index], &source, index, 2) == 0);
                total_cps += shards[index].target_cps;
                total_concurrency += shards[index].max_concurrency;
                shard_http = shards[index].classes[0].proto_config;
                shard_dns = shards[index].classes[1].proto_config;
                ASSERT_TRUE(shard_http != source_http);
                ASSERT_TRUE(shard_dns != source_dns);
                ASSERT_TRUE(strcmp(shard_http->method, "GET") == 0);
                ASSERT_TRUE(strcmp(shard_dns->qname, source_dns->qname) == 0);
                ASSERT_TRUE(shard_dns->qtype == source_dns->qtype);
        }
        ASSERT_TRUE(total_cps == source.target_cps);
        ASSERT_TRUE(total_concurrency == source.max_concurrency);
        for (unsigned int index = 0; index < 2; index++)
                tg_plan_fini(&shards[index]);
        tg_plan_fini(&source);
        return 0;
}

static int test_partition_clone_failure_cleanup(void) {
        struct tg_plan source = {0};
        struct tg_plan destination = {0};

        partition_clone_calls = 0;
        partition_config_free_calls = 0;
        partition_fail_at = 2;
        partition_source_free_during_failure = 0;
        partition_cleanup_in_progress = 1;

        source.target_cps = PARTITION_FAILURE_CLASS_COUNT;
        source.max_concurrency = PARTITION_FAILURE_CLASS_COUNT;
        source.total_weight = PARTITION_FAILURE_CLASS_COUNT;
        source.class_count = PARTITION_FAILURE_CLASS_COUNT;
        for (unsigned int index = 0; index < PARTITION_FAILURE_CLASS_COUNT;
             index++) {
                int *config = malloc(sizeof(*config));

                ASSERT_TRUE(config != NULL);
                *config = (int)index;
                partition_source_configs[index] = config;
                source.classes[index].proto = &partition_test_proto_ops;
                source.classes[index].proto_config = config;
        }

        errno = 0;
        ASSERT_TRUE(tg_plan_partition(&destination, &source, 0, 1) == -1);
        partition_cleanup_in_progress = 0;
        ASSERT_TRUE(errno == EIO);
        ASSERT_TRUE(partition_clone_calls == 2);
        ASSERT_TRUE(partition_config_free_calls == 2);
        ASSERT_TRUE(partition_source_free_during_failure == 0);
        ASSERT_TRUE(destination.class_count == 0);
        ASSERT_TRUE(destination.classes[0].proto_config == NULL);
        ASSERT_TRUE(destination.classes[1].proto_config == NULL);
        ASSERT_TRUE(destination.classes[2].proto_config == NULL);
        for (unsigned int index = 0; index < PARTITION_FAILURE_CLASS_COUNT;
             index++)
                ASSERT_TRUE(source.classes[index].proto_config ==
                            partition_source_configs[index]);

        tg_plan_fini(&source);
        ASSERT_TRUE(partition_config_free_calls ==
                    2 + PARTITION_FAILURE_CLASS_COUNT);
        memset(partition_source_configs, 0, sizeof(partition_source_configs));
        return 0;
}

static int test_protocol_transport_validation(void) {
        struct tg_plan plan;

        errno = 0;
        ASSERT_TRUE(tg_plan_load_file(
                        &plan, "fixtures/invalid_dns_transport.json") == -1);
        ASSERT_TRUE(errno == EINVAL);
        return 0;
}

static int test_plan_partition(void) {
        struct tg_plan source;
        struct tg_plan shards[3] = {0};
        const struct tg_http_config *source_config;
        uint32_t total_cps = 0;
        uint32_t total_concurrency = 0;

        ASSERT_TRUE(
            tg_plan_load_file(
                &source, "../traffic-gen/scenarios/bootstrap_http.json") == 0);
        source.target_cps = 10;
        source.max_concurrency = 3;
        source_config = source.classes[0].proto_config;
        ASSERT_TRUE(source_config != NULL);
        for (unsigned int index = 0; index < 3; index++) {
                const struct tg_http_config *shard_config;

                ASSERT_TRUE(
                    tg_plan_partition(&shards[index], &source, index, 3) == 0);
                total_cps += shards[index].target_cps;
                total_concurrency += shards[index].max_concurrency;
                shard_config = shards[index].classes[0].proto_config;
                ASSERT_TRUE(shard_config != NULL);
                ASSERT_TRUE(shard_config != source_config);
                ASSERT_TRUE(strcmp(shard_config->method, "GET") == 0);
                ASSERT_TRUE(strcmp(shard_config->path, "/") == 0);
                ASSERT_TRUE(shards[index].classes[0].proto_config ==
                            (void *)shard_config);
        }
        ASSERT_TRUE(shards[0].target_cps == 4);
        ASSERT_TRUE(shards[1].target_cps == 3);
        ASSERT_TRUE(shards[2].target_cps == 3);
        ASSERT_TRUE(total_cps == source.target_cps);
        ASSERT_TRUE(total_concurrency == source.max_concurrency);
        ASSERT_TRUE(shards[0].selection_phase == 0);
        ASSERT_TRUE(shards[1].selection_phase == 0);
        ASSERT_TRUE(shards[2].selection_phase == 0);
        errno = 0;
        ASSERT_TRUE(tg_plan_partition(&shards[0], &source, 3, 3) == -1);
        ASSERT_TRUE(errno == EINVAL);
        for (unsigned int index = 0; index < 3; index++)
                tg_plan_fini(&shards[index]);
        tg_plan_fini(&source);
        return 0;
}

static int test_active_shards_and_weight_phase(void) {
        struct tg_plan source;
        struct tg_plan shards[2] = {0};
        struct tg_scheduler schedulers[2];
        struct start_recorder recorders[2] = {0};

        memset(&source, 0, sizeof(source));
        source.duration_sec = 2;
        source.target_cps = 4;
        source.max_concurrency = 4;
        source.total_weight = 2;
        source.class_count = 2;
        source.classes[0].weight = 1;
        source.classes[1].weight = 1;
        strcpy(source.classes[0].name, "A");
        strcpy(source.classes[1].name, "B");

        ASSERT_TRUE(tg_plan_active_shards(&source, 0) == 0);
        ASSERT_TRUE(tg_plan_active_shards(&source, 1) == 1);
        ASSERT_TRUE(tg_plan_active_shards(&source, 8) == 4);
        errno = 0;
        ASSERT_TRUE(tg_plan_partition(&shards[0], &source, 0, 5) == -1);
        ASSERT_TRUE(errno == EINVAL);
        ASSERT_TRUE(tg_plan_partition(&shards[0], &source, 0, 2) == 0);
        ASSERT_TRUE(tg_plan_partition(&shards[1], &source, 1, 2) == 0);
        ASSERT_TRUE(shards[0].selection_phase == 0);
        ASSERT_TRUE(shards[1].selection_phase == 1);
        ASSERT_TRUE(tg_scheduler_init(&schedulers[0], &shards[0], 10) == 0);
        ASSERT_TRUE(tg_scheduler_init(&schedulers[1], &shards[1], 10) == 0);

        ASSERT_TRUE(tg_scheduler_tick(&schedulers[0], 0, 2, record_start,
                                      &recorders[0]) == 0);
        ASSERT_TRUE(tg_scheduler_tick(&schedulers[1], 0, 2, record_start,
                                      &recorders[1]) == 0);
        ASSERT_TRUE(tg_scheduler_tick(&schedulers[0], 10, 2, record_start,
                                      &recorders[0]) == 2);
        ASSERT_TRUE(tg_scheduler_tick(&schedulers[1], 10, 2, record_start,
                                      &recorders[1]) == 2);
        ASSERT_TRUE(memcmp(recorders[0].selected, "AB", 2) == 0);
        ASSERT_TRUE(memcmp(recorders[1].selected, "BA", 2) == 0);

        tg_plan_fini(&shards[0]);
        tg_plan_fini(&shards[1]);
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
        ASSERT_TRUE(test_mixed_protocol_plan_and_partition() == 0);
        ASSERT_TRUE(test_partition_clone_failure_cleanup() == 0);
        ASSERT_TRUE(test_protocol_transport_validation() == 0);
        ASSERT_TRUE(test_plan_partition() == 0);
        ASSERT_TRUE(test_active_shards_and_weight_phase() == 0);
        ASSERT_TRUE(test_weight_token_and_concurrency_bounds() == 0);
        ASSERT_TRUE(test_failed_start_and_duration_stop() == 0);
        ASSERT_TRUE(test_stats() == 0);
        return EXIT_SUCCESS;
}
