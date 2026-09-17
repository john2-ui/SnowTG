/**
 * @file app_args.c
 * @brief Parses traffic-generator application options independently of EAL.
 */

#include "app_args.h"

#include "../pro-stack/config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/** Parse one base-10 unsigned value within an inclusive range. */
static int tg_parse_unsigned(const char *text, unsigned long minimum,
                             unsigned long maximum, unsigned long *value_out) {
        char *end = NULL;
        unsigned long value;

        if (text == NULL || text[0] == '\0' || text[0] == '-' ||
            value_out == NULL) {
                errno = EINVAL;
                return -1;
        }
        errno = 0;
        value = strtoul(text, &end, 10);
        if (errno != 0 || end == text || *end != '\0' || value < minimum ||
            value > maximum) {
                errno = EINVAL;
                return -1;
        }
        *value_out = value;
        return 0;
}

/** @copydoc tg_app_config_parse */
int tg_app_config_parse(int argc, char *argv[],
                        struct tg_app_config *config_out) {
        struct tg_app_config config = {
            .scenario_path = TG_DEFAULT_SCENARIO_PATH,
            .worker_count = 1,
            .metrics_sample = 1024,
            .rx_mode = TG_RX_AUTO,
            .tx_mode = TG_TX_AUTO,
            .port_id = TG_DEFAULT_PORT_ID,
        };
        bool workers_seen = false;
        bool socket_id_max_seen = false;
        bool stats_csv_seen = false;
        bool dataplane_csv_seen = false;
        bool metrics_sample_seen = false;
        bool tx_mode_seen = false;
        bool rx_mode_seen = false;
        bool mtu_seen = false;
        bool local_ip_seen = false;
        bool port_id_seen = false;
        bool scenario_seen = false;

        if (argc < 1 || argv == NULL || config_out == NULL ||
            inet_pton(AF_INET, TG_DEFAULT_LOCAL_IP, &config.local_ip) != 1) {
                errno = EINVAL;
                return -1;
        }

        for (int i = 1; i < argc; i++) {
                unsigned long value;

                if (strcmp(argv[i], "--workers") == 0) {
                        if (workers_seen || ++i == argc ||
                            tg_parse_unsigned(argv[i], 1, UINT_MAX, &value) !=
                                0)
                                goto invalid;
                        config.worker_count = (unsigned int)value;
                        workers_seen = true;
                        continue;
                }
                if (strcmp(argv[i], "--socket-id-max") == 0) {
                        if (socket_id_max_seen || ++i == argc ||
                            tg_parse_unsigned(argv[i], 1, UINT32_MAX, &value) !=
                                0)
                                goto invalid;
                        config.socket_id_max_override = (uint32_t)value;
                        socket_id_max_seen = true;
                        continue;
                }
                if (strcmp(argv[i], "--stats-csv") == 0) {
                        if (stats_csv_seen || ++i == argc ||
                            argv[i][0] == '\0' || argv[i][0] == '-')
                                goto invalid;
                        config.stats_csv_path = argv[i];
                        stats_csv_seen = true;
                        continue;
                }
                if (strcmp(argv[i], "--dataplane-csv") == 0) {
                        if (dataplane_csv_seen || ++i == argc ||
                            argv[i][0] == '\0' || argv[i][0] == '-')
                                goto invalid;
                        config.dataplane_csv_path = argv[i];
                        dataplane_csv_seen = true;
                        continue;
                }
                if (strcmp(argv[i], "--metrics-sample") == 0) {
                        if (metrics_sample_seen || ++i == argc ||
                            tg_parse_unsigned(argv[i], 0, UINT32_MAX, &value) != 0)
                                goto invalid;
                        config.metrics_sample = (uint32_t)value;
                        metrics_sample_seen = true;
                        continue;
                }
                if (strcmp(argv[i], "--tx-mode") == 0) {
                        if (tx_mode_seen || ++i == argc)
                                goto invalid;
                        if (strcmp(argv[i], "main") == 0)
                                config.tx_mode = TG_TX_MAIN;
                        else if (strcmp(argv[i], "worker") == 0)
                                config.tx_mode = TG_TX_WORKER;
                        else if (strcmp(argv[i], "auto") == 0)
                                config.tx_mode = TG_TX_AUTO;
                        else
                                goto invalid;
                        tx_mode_seen = true;
                        continue;
                }
                if (strcmp(argv[i], "--rx-mode") == 0) {
                        if (rx_mode_seen || ++i == argc)
                                goto invalid;
                        if (strcmp(argv[i], "main") == 0)
                                config.rx_mode = TG_RX_MAIN;
                        else if (strcmp(argv[i], "worker") == 0)
                                config.rx_mode = TG_RX_WORKER;
                        else if (strcmp(argv[i], "auto") == 0)
                                config.rx_mode = TG_RX_AUTO;
                        else
                                goto invalid;
                        rx_mode_seen = true;
                        continue;
                }
                if (strcmp(argv[i], "--mtu") == 0) {
                        if (mtu_seen || ++i == argc ||
                            tg_parse_unsigned(argv[i], IPV4_MIN_MTU, UINT16_MAX,
                                              &value) != 0)
                                goto invalid;
                        config.requested_mtu = (uint16_t)value;
                        mtu_seen = true;
                        continue;
                }
                if (strcmp(argv[i], "--local-ip") == 0) {
                        if (local_ip_seen || ++i == argc ||
                            inet_pton(AF_INET, argv[i], &config.local_ip) != 1)
                                goto invalid;
                        local_ip_seen = true;
                        continue;
                }
                if (strcmp(argv[i], "--port-id") == 0) {
                        if (port_id_seen || ++i == argc ||
                            tg_parse_unsigned(argv[i], 0, UINT16_MAX, &value) !=
                                0)
                                goto invalid;
                        config.port_id = (uint16_t)value;
                        port_id_seen = true;
                        continue;
                }
                if (argv[i][0] == '-' || scenario_seen)
                        goto invalid;
                config.scenario_path = argv[i];
                scenario_seen = true;
        }

        if (config.stats_csv_path != NULL && config.dataplane_csv_path != NULL &&
            strcmp(config.stats_csv_path, config.dataplane_csv_path) == 0)
                goto invalid;
        *config_out = config;
        return 0;

invalid:
        errno = EINVAL;
        return -1;
}
