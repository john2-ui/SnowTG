#include "port.h"

#include "config.h"
#include "log.h"

#include <rte_eal.h>
#include <rte_ethdev.h>

void port_init(uint16_t port_id, struct rte_mempool *mp) {
        uint16_t nb_sys_ports = rte_eth_dev_count_avail();
        if (nb_sys_ports == 0)
                rte_exit(EXIT_FAILURE, "no available ethernet ports\n");

        struct rte_eth_dev_info dev_info;
        if (rte_eth_dev_info_get(port_id, &dev_info) != 0)
                rte_exit(EXIT_FAILURE, "rte_eth_dev_info_get(%u) failed\n",
                         port_id);

        const uint16_t num_rx_queues = 1;
        const uint16_t num_tx_queues = 1;

        struct rte_eth_conf port_conf = {.rxmode = {.mtu = RTE_ETHER_MTU}};

        if (rte_eth_dev_configure(port_id, num_rx_queues, num_tx_queues,
                                  &port_conf) < 0)
                rte_exit(EXIT_FAILURE, "rte_eth_dev_configure(%u) failed\n",
                         port_id);

        if (rte_eth_rx_queue_setup(port_id, 0, NB_DESC,
                                   rte_eth_dev_socket_id(port_id), NULL,
                                   mp) < 0)
                rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup(%u) failed\n",
                         port_id);

#if ENABLE_SEND
        struct rte_eth_txconf txq_conf = dev_info.default_txconf;
        txq_conf.offloads = port_conf.txmode.offloads;
        if (rte_eth_tx_queue_setup(port_id, 0, NB_DESC,
                                   rte_eth_dev_socket_id(port_id),
                                   &txq_conf) < 0)
                rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup(%u) failed\n",
                         port_id);
#endif

        if (rte_eth_dev_start(port_id) < 0)
                rte_exit(EXIT_FAILURE, "rte_eth_dev_start(%u) failed\n",
                         port_id);

        LOG_INFO("port %u started (driver=%s)", port_id, dev_info.driver_name);
}
