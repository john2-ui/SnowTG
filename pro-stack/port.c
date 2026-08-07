#include "port.h"

#include "config.h"
#include "log.h"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_thash.h>

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

static int port_rss_configure_reta(uint16_t port_id, uint16_t queue_count,
                                   uint16_t reta_size) {
        uint16_t group_count;
        struct rte_eth_rss_reta_entry64 *reta_conf;

        if (reta_size == 0)
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
        g_rss.reta = calloc(reta_size, sizeof(*g_rss.reta));
        if (g_rss.reta == NULL) {
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

void port_init_queues(uint16_t port_id, struct rte_mempool *mp,
                      uint16_t queue_count) {
        uint16_t nb_sys_ports = rte_eth_dev_count_avail();
        if (nb_sys_ports == 0)
                rte_exit(EXIT_FAILURE, "no available ethernet ports\n");
        if (mp == NULL || queue_count == 0)
                rte_exit(EXIT_FAILURE, "invalid port queue configuration\n");

        struct rte_eth_dev_info dev_info;
        if (rte_eth_dev_info_get(port_id, &dev_info) != 0)
                rte_exit(EXIT_FAILURE, "rte_eth_dev_info_get(%u) failed\n",
                         port_id);

        if (queue_count > dev_info.max_rx_queues ||
            queue_count > dev_info.max_tx_queues)
                rte_exit(EXIT_FAILURE,
                         "port %u supports at most %u RX and %u TX queues; "
                         "%u requested\n",
                         port_id, dev_info.max_rx_queues,
                         dev_info.max_tx_queues, queue_count);

        struct rte_eth_conf port_conf = {.rxmode = {.mtu = RTE_ETHER_MTU}};
        port_rss_reset();
        if (queue_count > 1) {
                const uint64_t rss_tcp = RTE_ETH_RSS_NONFRAG_IPV4_TCP;
                const uint64_t rss_udp = RTE_ETH_RSS_NONFRAG_IPV4_UDP;
                uint64_t rss_hf = rss_tcp;

                if ((dev_info.flow_type_rss_offloads & rss_tcp) != rss_tcp)
                        rte_exit(EXIT_FAILURE,
                                 "port %u lacks IPv4 TCP RSS support\n",
                                 port_id);
                if ((dev_info.flow_type_rss_offloads & rss_udp) == rss_udp)
                        rss_hf |= rss_udp;
                if (dev_info.hash_key_size == 0 ||
                    dev_info.hash_key_size > sizeof(g_rss.key))
                        rte_exit(EXIT_FAILURE,
                                 "port %u has unsupported RSS key size %u\n",
                                 port_id, dev_info.hash_key_size);
                memcpy(g_rss.key, port_rss_default_key,
                       dev_info.hash_key_size < sizeof(port_rss_default_key)
                           ? dev_info.hash_key_size
                           : sizeof(port_rss_default_key));
                g_rss.key_len = dev_info.hash_key_size;
                port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
                port_conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;
                port_conf.rx_adv_conf.rss_conf.rss_key = g_rss.key;
                port_conf.rx_adv_conf.rss_conf.rss_key_len = g_rss.key_len;
        }

        if (rte_eth_dev_configure(port_id, queue_count, queue_count,
                                  &port_conf) < 0)
                rte_exit(EXIT_FAILURE, "rte_eth_dev_configure(%u) failed\n",
                         port_id);

        for (uint16_t queue = 0; queue < queue_count; queue++) {
                if (rte_eth_rx_queue_setup(port_id, queue, NB_DESC,
                                           rte_eth_dev_socket_id(port_id), NULL,
                                           mp) < 0)
                        rte_exit(EXIT_FAILURE,
                                 "rte_eth_rx_queue_setup(%u, %u) failed\n",
                                 port_id, queue);
        }

#if ENABLE_SEND
        struct rte_eth_txconf txq_conf = dev_info.default_txconf;
        txq_conf.offloads = port_conf.txmode.offloads;
        for (uint16_t queue = 0; queue < queue_count; queue++) {
                if (rte_eth_tx_queue_setup(port_id, queue, NB_DESC,
                                           rte_eth_dev_socket_id(port_id),
                                           &txq_conf) < 0)
                        rte_exit(EXIT_FAILURE,
                                 "rte_eth_tx_queue_setup(%u, %u) failed\n",
                                 port_id, queue);
        }
#endif

        if (rte_eth_dev_start(port_id) < 0)
                rte_exit(EXIT_FAILURE, "rte_eth_dev_start(%u) failed\n",
                         port_id);
        if (queue_count > 1) {
                if (port_rss_configure_reta(port_id, queue_count,
                                            dev_info.reta_size) != 0)
                        rte_exit(EXIT_FAILURE,
                                 "port %u RSS RETA configuration failed\n",
                                 port_id);
                g_rss.enabled = true;
                g_rss.port_id = port_id;
                g_rss.queue_count = queue_count;
                g_rss.reta_size = dev_info.reta_size;
        }

        LOG_INFO("port %u started (driver=%s queues=%u rss=%s)", port_id,
                 dev_info.driver_name, queue_count,
                 queue_count > 1 ? ((dev_info.flow_type_rss_offloads &
                                     RTE_ETH_RSS_NONFRAG_IPV4_UDP)
                                        ? "ipv4-tcp-udp"
                                        : "ipv4-tcp")
                                 : "off");
}

void port_init(uint16_t port_id, struct rte_mempool *mp) {
        port_init_queues(port_id, mp, 1);
}

int port_rss_queue_for_tcp(uint32_t remote_ip, uint32_t local_ip,
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
