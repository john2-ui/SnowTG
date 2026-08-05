#include "port.h"

#include "config.h"
#include "log.h"

#include <rte_eal.h>
#include <rte_ethdev.h>

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
        if (queue_count > 1) {
                const uint64_t rss_hf = RTE_ETH_RSS_NONFRAG_IPV4_TCP |
                                        RTE_ETH_RSS_NONFRAG_IPV4_UDP;

                if ((dev_info.flow_type_rss_offloads & rss_hf) != rss_hf)
                        rte_exit(EXIT_FAILURE,
                                 "port %u lacks IPv4 TCP/UDP RSS support\n",
                                 port_id);
                port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
                port_conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;
        }

        if (rte_eth_dev_configure(port_id, queue_count, queue_count,
                                  &port_conf) < 0)
                rte_exit(EXIT_FAILURE, "rte_eth_dev_configure(%u) failed\n",
                         port_id);

        for (uint16_t queue = 0; queue < queue_count; queue++) {
                if (rte_eth_rx_queue_setup(port_id, queue, NB_DESC,
                                           rte_eth_dev_socket_id(port_id),
                                           NULL, mp) < 0)
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

        LOG_INFO("port %u started (driver=%s queues=%u rss=%s)", port_id,
                 dev_info.driver_name, queue_count,
                 queue_count > 1 ? "ipv4-tcp-udp" : "off");
}

void port_init(uint16_t port_id, struct rte_mempool *mp) {
        port_init_queues(port_id, mp, 1);
}
