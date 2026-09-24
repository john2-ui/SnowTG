#include "../traffic-gen/app_args.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ASSERT_TRUE(condition)                                                 \
        do {                                                                   \
                if (!(condition)) {                                            \
                        fprintf(stderr, "%s:%d assertion failed: %s\n",        \
                                __FILE__, __LINE__, #condition);               \
                        return -1;                                             \
                }                                                              \
        } while (0)

static int test_defaults(void) {
        char *argv[] = {"traffic-gen"};
        struct tg_app_config config;
        struct in_addr expected_ip;

        ASSERT_TRUE(tg_app_config_parse(1, argv, &config) == 0);
        ASSERT_TRUE(config.worker_count == 1);
        ASSERT_TRUE(config.socket_id_max_override == 0);
        ASSERT_TRUE(config.stats_csv_path == NULL);
        ASSERT_TRUE(config.latency_csv_path == NULL);
        ASSERT_TRUE(config.dataplane_csv_path == NULL);
        ASSERT_TRUE(config.metrics_sample == 1024);
        ASSERT_TRUE(config.tx_mode == TG_TX_AUTO);
        ASSERT_TRUE(config.rx_mode == TG_RX_AUTO);
        ASSERT_TRUE(config.requested_mtu == 0);
        ASSERT_TRUE(config.port_id == TG_DEFAULT_PORT_ID);
        ASSERT_TRUE(strcmp(config.scenario_path, TG_DEFAULT_SCENARIO_PATH) ==
                    0);
        ASSERT_TRUE(inet_pton(AF_INET, TG_DEFAULT_LOCAL_IP, &expected_ip) == 1);
        ASSERT_TRUE(config.local_ip == expected_ip.s_addr);
        return 0;
}

static int test_overrides_and_order(void) {
        char *argv[] = {
            "traffic-gen",     "custom.json", "--port-id",   "65535",
            "--workers",       "4",           "--local-ip",  "10.20.30.40",
            "--mtu",           "1500",        "--stats-csv", "results.csv",
            "--socket-id-max", "8192", "--dataplane-csv", "main.csv",
            "--metrics-sample", "0", "--tx-mode", "worker", "--rx-mode", "main", "--latency-csv", "latency.csv"};
        struct tg_app_config config;
        struct in_addr expected_ip;

        ASSERT_TRUE(tg_app_config_parse((int)(sizeof(argv) / sizeof(argv[0])),
                                        argv, &config) == 0);
        ASSERT_TRUE(config.worker_count == 4);
        ASSERT_TRUE(strcmp(config.latency_csv_path, "latency.csv") == 0);
        ASSERT_TRUE(config.socket_id_max_override == 8192);
        ASSERT_TRUE(strcmp(config.stats_csv_path, "results.csv") == 0);
        ASSERT_TRUE(config.requested_mtu == 1500);
        ASSERT_TRUE(strcmp(config.dataplane_csv_path, "main.csv") == 0);
        ASSERT_TRUE(config.metrics_sample == 0);
        ASSERT_TRUE(config.tx_mode == TG_TX_WORKER);
        ASSERT_TRUE(config.rx_mode == TG_RX_MAIN);
        ASSERT_TRUE(config.port_id == UINT16_MAX);
        ASSERT_TRUE(strcmp(config.scenario_path, "custom.json") == 0);
        ASSERT_TRUE(inet_pton(AF_INET, "10.20.30.40", &expected_ip) == 1);
        ASSERT_TRUE(config.local_ip == expected_ip.s_addr);
        return 0;
}

static int expect_invalid(int argc, char *argv[]) {
        struct tg_app_config config;

        errno = 0;
        ASSERT_TRUE(tg_app_config_parse(argc, argv, &config) == -1);
        ASSERT_TRUE(errno == EINVAL);
        return 0;
}

static int test_invalid_options(void) {
        char *duplicate_ip[] = {"traffic-gen", "--local-ip", "10.0.0.1",
                                "--local-ip", "10.0.0.2"};
        char *duplicate_port[] = {"traffic-gen", "--port-id", "0", "--port-id",
                                  "1"};
        char *missing_ip[] = {"traffic-gen", "--local-ip"};
        char *missing_port[] = {"traffic-gen", "--port-id"};
        char *missing_stats[] = {"traffic-gen", "--stats-csv", "--workers",
                                 "1"};
        char *bad_ip[] = {"traffic-gen", "--local-ip", "10.0.0.999"};
        char *negative_port[] = {"traffic-gen", "--port-id", "-1"};
        char *text_port[] = {"traffic-gen", "--port-id", "port0"};
        char *large_port[] = {"traffic-gen", "--port-id", "65536"};
        char *unknown[] = {"traffic-gen", "--interface", "0"};
        char *two_scenarios[] = {"traffic-gen", "one.json", "two.json"};
        char *duplicate_workers[] = {"traffic-gen", "--workers", "1",
                                     "--workers", "2"};
        char *missing_latency[] = {"traffic-gen", "--latency-csv"};
        char *duplicate_latency[] = {"traffic-gen", "--latency-csv", "a", "--latency-csv", "b"};
        char *same_latency[] = {"traffic-gen", "--stats-csv", "a", "--latency-csv", "a"};
        char *same_dataplane[] = {"traffic-gen", "--dataplane-csv", "a", "--latency-csv", "a"};
        char *same_csv[] = {"traffic-gen", "--stats-csv", "out.csv",
                            "--dataplane-csv", "out.csv"};
        char *bad_sample[] = {"traffic-gen", "--metrics-sample", "-1"};
        char *missing_sample[] = {"traffic-gen", "--metrics-sample"};
        char *duplicate_sample[] = {"traffic-gen", "--metrics-sample", "64",
                                     "--metrics-sample", "1"};
        char *bad_tx[] = {"traffic-gen", "--tx-mode", "shared"};
        char *duplicate_tx[] = {"traffic-gen", "--tx-mode", "main", "--tx-mode", "worker"};

        char *bad_rx[] = {"traffic-gen", "--rx-mode", "shared"};
        char *duplicate_rx[] = {"traffic-gen", "--rx-mode", "main", "--rx-mode", "worker"};

#define EXPECT_INVALID(args)                                                   \
        ASSERT_TRUE(expect_invalid((int)(sizeof(args) / sizeof((args)[0])),    \
                                   (args)) == 0)
        EXPECT_INVALID(duplicate_ip);
        EXPECT_INVALID(duplicate_port);
        EXPECT_INVALID(missing_ip);
        EXPECT_INVALID(missing_port);
        EXPECT_INVALID(missing_stats);
        EXPECT_INVALID(bad_ip);
        EXPECT_INVALID(negative_port);
        EXPECT_INVALID(text_port);
        EXPECT_INVALID(large_port);
        EXPECT_INVALID(unknown);
        EXPECT_INVALID(two_scenarios);
        EXPECT_INVALID(duplicate_workers);
        EXPECT_INVALID(same_csv);
        EXPECT_INVALID(missing_latency);
        EXPECT_INVALID(duplicate_latency);
        EXPECT_INVALID(same_latency);
        EXPECT_INVALID(same_dataplane);
        EXPECT_INVALID(bad_sample);
        EXPECT_INVALID(missing_sample);
        EXPECT_INVALID(duplicate_sample);
        EXPECT_INVALID(bad_rx);
        EXPECT_INVALID(duplicate_rx);
        EXPECT_INVALID(bad_tx);
        EXPECT_INVALID(duplicate_tx);
#undef EXPECT_INVALID
        return 0;
}

int main(void) {
        if (test_defaults() != 0 || test_overrides_and_order() != 0 ||
            test_invalid_options() != 0)
                return 1;
        puts("app argument tests passed");
        return 0;
}
