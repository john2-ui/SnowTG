#include "port.h"

#include "config.h"
#include "log.h"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_thash.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct port_rss_state {
        bool enabled;
        uint16_t port_id;
        uint16_t queue_count;
        uint16_t reta_size;
        uint8_t key[RTE_THASH_KEY_LEN_MAX];
        uint8_t key_len;
        uint16_t *reta;
};

static struct port_rss_state g_rss;
static struct port_topology g_topology;

static const uint8_t port_rss_default_key[] = {
    0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2, 0x41, 0x67,
    0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0, 0xd0, 0xca, 0x2b, 0xcb,
    0xae, 0x7b, 0x30, 0xb4, 0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30,
    0xf2, 0x0c, 0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa,
};

static void port_rss_reset(void) {
        free(g_rss.reta);
        memset(&g_rss, 0, sizeof(g_rss));
}

static int port_rss_prepare_key(uint16_t key_len) {
        if (key_len == 0 || key_len > sizeof(g_rss.key))
                return -1;

        memset(g_rss.key, 0, sizeof(g_rss.key));
        memcpy(g_rss.key, port_rss_default_key,
               key_len < sizeof(port_rss_default_key)
                   ? key_len
                   : sizeof(port_rss_default_key));
        g_rss.key_len = (uint8_t)key_len;
        return 0;
}

static int port_rss_allocate_reta(uint16_t queue_count, uint16_t reta_size) {
        if (queue_count == 0 || reta_size == 0)
                return -1;

        g_rss.reta = calloc(reta_size, sizeof(*g_rss.reta));
        if (g_rss.reta == NULL)
                return -1;
        for (uint16_t entry = 0; entry < reta_size; entry++)
                g_rss.reta[entry] = entry % queue_count;
        g_rss.queue_count = queue_count;
        g_rss.reta_size = reta_size;
        return 0;
}

static int port_rss_configure_reta(uint16_t port_id, uint16_t queue_count,
                                   uint16_t reta_size) {
        uint16_t group_count;
        struct rte_eth_rss_reta_entry64 *reta_conf;

        if (queue_count == 0 || reta_size == 0)
                return -1;
        group_count = (uint16_t)((reta_size + RTE_ETH_RETA_GROUP_SIZE - 1U) /
                                 RTE_ETH_RETA_GROUP_SIZE);
        reta_conf = calloc(group_count, sizeof(*reta_conf));
        if (reta_conf == NULL)
                return -1;
        for (uint16_t entry = 0; entry < reta_size; entry++) {
                uint16_t group = entry / RTE_ETH_RETA_GROUP_SIZE;
                uint16_t offset = entry % RTE_ETH_RETA_GROUP_SIZE;

                reta_conf[group].mask |= UINT64_C(1) << offset;
                reta_conf[group].reta[offset] = entry % queue_count;
        }
        if (rte_eth_dev_rss_reta_update(port_id, reta_conf, reta_size) != 0) {
                free(reta_conf);
                return -1;
        }
        if (rte_eth_dev_rss_reta_query(port_id, reta_conf, reta_size) != 0) {
                free(reta_conf);
                return -1;
        }
        if (port_rss_allocate_reta(queue_count, reta_size) != 0) {
                free(reta_conf);
                return -1;
        }
        for (uint16_t entry = 0; entry < reta_size; entry++) {
                uint16_t queue = reta_conf[entry / RTE_ETH_RETA_GROUP_SIZE]
                                     .reta[entry % RTE_ETH_RETA_GROUP_SIZE];

                if (queue >= queue_count) {
                        free(reta_conf);
                        port_rss_reset();
                        return -1;
                }
                g_rss.reta[entry] = queue;
        }
        free(reta_conf);
        return 0;
}

enum port_setup_failure {
        PORT_SETUP_OK,
        PORT_SETUP_CONFIGURE,
        PORT_SETUP_RX_QUEUE,
        PORT_SETUP_TX_QUEUE,
        PORT_SETUP_START,
};

static enum port_setup_failure
port_setup(uint16_t port_id, struct rte_mempool *mp,
           const struct rte_eth_dev_info *dev_info, uint16_t rx_queue_count,
           uint16_t tx_queue_count, bool enable_rss, uint64_t rss_hf) {
        struct rte_eth_conf port_conf = {.rxmode = {.mtu = RTE_ETHER_MTU}};

        if (enable_rss) {
                port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
                port_conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;
                port_conf.rx_adv_conf.rss_conf.rss_key = g_rss.key;
                port_conf.rx_adv_conf.rss_conf.rss_key_len = g_rss.key_len;
        }
        if (rte_eth_dev_configure(port_id, rx_queue_count, tx_queue_count,
                                  &port_conf) < 0)
                return PORT_SETUP_CONFIGURE;

        for (uint16_t queue = 0; queue < rx_queue_count; queue++) {
                if (rte_eth_rx_queue_setup(port_id, queue, NB_DESC,
                                           rte_eth_dev_socket_id(port_id), NULL,
                                           mp) < 0)
                        return PORT_SETUP_RX_QUEUE;
        }

#if ENABLE_SEND
        struct rte_eth_txconf txq_conf = dev_info->default_txconf;
        txq_conf.offloads = port_conf.txmode.offloads;
        for (uint16_t queue = 0; queue < tx_queue_count; queue++) {
                if (rte_eth_tx_queue_setup(port_id, queue, NB_DESC,
                                           rte_eth_dev_socket_id(port_id),
                                           &txq_conf) < 0)
                        return PORT_SETUP_TX_QUEUE;
        }
#endif

        if (rte_eth_dev_start(port_id) < 0)
                return PORT_SETUP_START;
        return PORT_SETUP_OK;
}

static void port_setup_cleanup(uint16_t port_id) {
        (void)rte_eth_dev_stop(port_id);
        (void)rte_eth_dev_close(port_id);
}

static const char *port_setup_failure_name(enum port_setup_failure failure) {
        switch (failure) {
        case PORT_SETUP_CONFIGURE:
                return "device configuration";
        case PORT_SETUP_RX_QUEUE:
                return "RX queue setup";
        case PORT_SETUP_TX_QUEUE:
                return "TX queue setup";
        case PORT_SETUP_START:
                return "device start";
        case PORT_SETUP_OK:
        default:
                return "unknown";
        }
}

static int port_rss_init_software(uint16_t worker_count) {
        /*
         * A fixed software RETA keeps Toeplitz mapping stable across NICs.
         * It need not match an unavailable hardware RETA.
         */
        enum { PORT_SOFTWARE_RETA_SIZE = 128 };

        if (port_rss_prepare_key(sizeof(port_rss_default_key)) != 0 ||
            port_rss_allocate_reta(worker_count, PORT_SOFTWARE_RETA_SIZE) != 0)
                return -1;
        g_rss.enabled = true;
        return 0;
}

struct port_topology port_init_queues(uint16_t port_id, struct rte_mempool *mp,
                                      uint16_t worker_count) {
        struct port_topology topology = {0};
        uint16_t nb_sys_ports = rte_eth_dev_count_avail();
        enum port_setup_failure failure;
        const uint64_t rss_tcp = RTE_ETH_RSS_NONFRAG_IPV4_TCP;
        const uint64_t rss_udp = RTE_ETH_RSS_NONFRAG_IPV4_UDP;
        uint64_t rss_hf = rss_tcp;
        const char *fallback_reason = NULL;

        if (nb_sys_ports == 0)
                rte_exit(EXIT_FAILURE, "no available ethernet ports\n");
        if (mp == NULL || worker_count == 0)
                rte_exit(EXIT_FAILURE, "invalid port queue configuration\n");

        struct rte_eth_dev_info dev_info;
        if (rte_eth_dev_info_get(port_id, &dev_info) != 0)
                rte_exit(EXIT_FAILURE, "rte_eth_dev_info_get(%u) failed\n",
                         port_id);

        if (dev_info.max_rx_queues == 0 || dev_info.max_tx_queues == 0)
                rte_exit(EXIT_FAILURE,
                         "port %u has no usable RX/TX queue (rx=%u tx=%u)\n",
                         port_id, dev_info.max_rx_queues,
                         dev_info.max_tx_queues);

        port_rss_reset();

        if (worker_count == 1) {
                failure = port_setup(port_id, mp, &dev_info, 1, 1, false, 0);
                if (failure != PORT_SETUP_OK)
                        rte_exit(EXIT_FAILURE, "port %u %s failed\n", port_id,
                                 port_setup_failure_name(failure));
                topology.rx_mode = PORT_RX_MODE_SINGLE_QUEUE;
                topology.rx_queue_count = 1;
                topology.tx_queue_count = 1;
                topology.worker_count = 1;
        } else {
#ifdef PORT_TESTING
                if (getenv("PORT_TEST_FORCE_SOFTWARE_RSS") != NULL)
                        fallback_reason = "test forced RSS unavailability";
                else
#endif
                if ((dev_info.flow_type_rss_offloads & rss_tcp) != rss_tcp)
                        fallback_reason = "IPv4 TCP RSS is unavailable";
                else if (worker_count > dev_info.max_rx_queues ||
                         worker_count > dev_info.max_tx_queues)
                        fallback_reason = "requested workers exceed RSS queues";
                else if (dev_info.reta_size == 0)
                        fallback_reason = "RSS RETA is unavailable";
                else if (port_rss_prepare_key(dev_info.hash_key_size) != 0)
                        fallback_reason = "RSS key size is unsupported";

                if (fallback_reason == NULL) {
                        if ((dev_info.flow_type_rss_offloads & rss_udp) ==
                            rss_udp)
                                rss_hf |= rss_udp;
                        failure =
                            port_setup(port_id, mp, &dev_info, worker_count,
                                       worker_count, true, rss_hf);
                        if (failure == PORT_SETUP_OK &&
                            port_rss_configure_reta(port_id, worker_count,
                                                    dev_info.reta_size) == 0) {
                                g_rss.enabled = true;
                                g_rss.port_id = port_id;
                                topology.rx_mode = PORT_RX_MODE_HARDWARE_RSS;
                                topology.rx_queue_count = worker_count;
                                topology.tx_queue_count = worker_count;
                                topology.worker_count = worker_count;
                                topology.rss_hf = rss_hf;
                        } else {
                                if (failure != PORT_SETUP_OK &&
                                    failure != PORT_SETUP_CONFIGURE &&
                                    failure != PORT_SETUP_START)
                                        rte_exit(
                                            EXIT_FAILURE,
                                            "port %u %s failed; refusing RSS "
                                            "fallback\n",
                                            port_id,
                                            port_setup_failure_name(failure));
                                fallback_reason =
                                    failure == PORT_SETUP_OK
                                        ? "RSS RETA setup failed"
                                        : port_setup_failure_name(failure);
                                port_setup_cleanup(port_id);
                                port_rss_reset();
                        }
                }

                if (topology.rx_mode != PORT_RX_MODE_HARDWARE_RSS) {
                        uint16_t tx_queue_count =
                            worker_count < dev_info.max_tx_queues
                                ? worker_count
                                : dev_info.max_tx_queues;

                        if (port_rss_init_software(worker_count) != 0)
                                rte_exit(EXIT_FAILURE,
                                         "software RSS state allocation "
                                         "failed\n");
                        failure = port_setup(port_id, mp, &dev_info, 1,
                                             tx_queue_count, false, 0);
                        if (failure != PORT_SETUP_OK)
                                rte_exit(EXIT_FAILURE,
                                         "port %u software fallback %s "
                                         "failed\n",
                                         port_id,
                                         port_setup_failure_name(failure));
                        topology.rx_mode = PORT_RX_MODE_SOFTWARE_DISPATCH;
                        topology.rx_queue_count = 1;
                        topology.tx_queue_count = tx_queue_count;
                        topology.worker_count = worker_count;
                        topology.rss_hf = rss_tcp | rss_udp;
                        LOG_INFO("port %u hardware RSS fallback: %s", port_id,
                                 fallback_reason == NULL
                                     ? "RSS setup did not complete"
                                     : fallback_reason);
                }
        }

        g_topology = topology;
        LOG_INFO("port %u started (driver=%s rx_mode=%s rxq=%u txq=%u "
                 "workers=%u rss_hf=0x%" PRIx64 ")",
                 port_id, dev_info.driver_name,
                 topology.rx_mode == PORT_RX_MODE_HARDWARE_RSS ? "hardware-rss"
                 : topology.rx_mode == PORT_RX_MODE_SOFTWARE_DISPATCH
                     ? "software-dispatch"
                     : "single-queue",
                 topology.rx_queue_count, topology.tx_queue_count,
                 topology.worker_count, topology.rss_hf);
        return topology;
}

void port_init(uint16_t port_id, struct rte_mempool *mp) {
        (void)port_init_queues(port_id, mp, 1);
}

int port_flow_queue_for_ipv4(__attribute__((unused)) uint8_t protocol,
                             uint32_t remote_ip, uint32_t local_ip,
                             uint16_t remote_port, uint16_t local_port) {
        uint32_t tuple[3];
        uint32_t hash;

        if (!g_rss.enabled || g_rss.reta == NULL || g_rss.reta_size == 0)
                return -1;
        tuple[0] = rte_be_to_cpu_32(remote_ip);
        tuple[1] = rte_be_to_cpu_32(local_ip);
        tuple[2] = ((uint32_t)rte_be_to_cpu_16(remote_port) << 16) |
                   rte_be_to_cpu_16(local_port);
        hash = rte_softrss(tuple, RTE_DIM(tuple), g_rss.key);
        return g_rss.reta[hash % g_rss.reta_size];
}

bool port_rx_uses_hardware_rss(uint8_t protocol) {
        uint64_t rss_protocol;

        if (g_topology.rx_mode != PORT_RX_MODE_HARDWARE_RSS)
                return false;
        if (protocol == IPPROTO_TCP)
                rss_protocol = RTE_ETH_RSS_NONFRAG_IPV4_TCP;
        else if (protocol == IPPROTO_UDP)
                rss_protocol = RTE_ETH_RSS_NONFRAG_IPV4_UDP;
        else
                return false;
        return (g_topology.rss_hf & rss_protocol) == rss_protocol;
}

int port_rss_queue_for_tcp(uint32_t remote_ip, uint32_t local_ip,
                           uint16_t remote_port, uint16_t local_port) {
        return port_flow_queue_for_ipv4(IPPROTO_TCP, remote_ip, local_ip,
                                        remote_port, local_port);
}

#ifdef PORT_TESTING
int port_test_configure_software_rss(uint16_t worker_count) {
        port_rss_reset();
        memset(&g_topology, 0, sizeof(g_topology));
        if (worker_count == 0 || port_rss_init_software(worker_count) != 0)
                return -1;
        g_topology.rx_mode = PORT_RX_MODE_SOFTWARE_DISPATCH;
        g_topology.rx_queue_count = 1;
        g_topology.tx_queue_count = worker_count;
        g_topology.worker_count = worker_count;
        g_topology.rss_hf =
            RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_NONFRAG_IPV4_UDP;
        return 0;
}

int port_test_configure_hardware_rss(uint16_t worker_count, uint64_t rss_hf) {
        port_rss_reset();
        memset(&g_topology, 0, sizeof(g_topology));
        if (worker_count == 0 ||
            port_rss_prepare_key(sizeof(port_rss_default_key)) != 0 ||
            port_rss_allocate_reta(worker_count, 128) != 0)
                return -1;
        g_rss.enabled = true;
        g_topology.rx_mode = PORT_RX_MODE_HARDWARE_RSS;
        g_topology.rx_queue_count = worker_count;
        g_topology.tx_queue_count = worker_count;
        g_topology.worker_count = worker_count;
        g_topology.rss_hf = rss_hf;
        return 0;
}

void port_test_reset(void) {
        port_rss_reset();
        memset(&g_topology, 0, sizeof(g_topology));
}
#endif
