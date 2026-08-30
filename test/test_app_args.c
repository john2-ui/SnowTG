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
            "--socket-id-max", "8192"};
        struct tg_app_config config;
        struct in_addr expected_ip;

        ASSERT_TRUE(tg_app_config_parse((int)(sizeof(argv) / sizeof(argv[0])),
                                        argv, &config) == 0);
        ASSERT_TRUE(config.worker_count == 4);
        ASSERT_TRUE(config.socket_id_max_override == 8192);
        ASSERT_TRUE(strcmp(config.stats_csv_path, "results.csv") == 0);
        ASSERT_TRUE(config.requested_mtu == 1500);
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
