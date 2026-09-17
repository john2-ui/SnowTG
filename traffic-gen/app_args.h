#ifndef TRAFFIC_GEN_APP_ARGS_H
#define TRAFFIC_GEN_APP_ARGS_H

/**
 * @file app_args.h
 * @brief Application options parsed after DPDK EAL initialization.
 */

#include <stdint.h>

/** Scenario loaded when no application scenario path is supplied. */
#define TG_DEFAULT_SCENARIO_PATH "scenarios/bootstrap_http.json"
/** Default local IPv4 identity retained for backward compatibility. */
#define TG_DEFAULT_LOCAL_IP "192.168.21.2"
/** Default DPDK ethernet port retained for backward compatibility. */
#define TG_DEFAULT_PORT_ID UINT16_C(0)

enum tg_tx_mode { TG_TX_MAIN, TG_TX_WORKER, TG_TX_AUTO };
enum tg_rx_mode { TG_RX_MAIN, TG_RX_WORKER, TG_RX_AUTO };

/** Validated traffic-generator application configuration. */
struct tg_app_config {
        const char *scenario_path;
        const char *stats_csv_path;
        const char *dataplane_csv_path;
        uint32_t metrics_sample; /**< Time one in N main loops; zero disables. */
        enum tg_tx_mode tx_mode;
        enum tg_rx_mode rx_mode;
        unsigned int worker_count;
        uint32_t socket_id_max_override;
        uint32_t local_ip; /**< Network byte order. */
        uint16_t port_id;
        uint16_t requested_mtu;
};

/**
 * Parse traffic-generator arguments after EAL has consumed its options.
 *
 * @param argc Application argument count, including argv[0].
 * @param argv Application argument vector.
 * @param config_out Destination receiving defaults and explicit overrides.
 * @return 0 on success; -1 with errno set to EINVAL on invalid input.
 */
int tg_app_config_parse(int argc, char *argv[],
                        struct tg_app_config *config_out);

#endif /* TRAFFIC_GEN_APP_ARGS_H */
