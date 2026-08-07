#include "../pro-stack/port.h"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                       \
        do {                                                                   \
                if (!(condition)) {                                            \
                        fprintf(stderr, "check failed: %s (%s:%d)\n",          \
                                #condition, __FILE__, __LINE__);               \
                        return 1;                                              \
                }                                                              \
        } while (0)

int main(void) {
        char *eal_argv[] = {
            "test_port_topology",
            "--no-huge",
            "--no-pci",
            "--vdev=net_null0",
            "-l",
            "0",
            "--file-prefix=port-topology-test",
            NULL,
        };
        struct rte_mempool *mp;
        struct port_topology topology;

        CHECK(setenv("PORT_TEST_FORCE_SOFTWARE_RSS", "1", 1) == 0);
        CHECK(rte_eal_init(7, eal_argv) >= 0);
        mp =
            rte_pktmbuf_pool_create("port_topology_mp", 1023, 0, 0,
                                    RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        CHECK(mp != NULL);

        topology = port_init_queues(0, mp, 2);
        CHECK(topology.rx_mode == PORT_RX_MODE_SOFTWARE_DISPATCH);
        CHECK(topology.rx_queue_count == 1);
        CHECK(topology.tx_queue_count >= 1);
        CHECK(topology.tx_queue_count <= 2);
        CHECK(topology.worker_count == 2);

        (void)rte_eth_dev_stop(0);
        (void)rte_eth_dev_close(0);
        rte_mempool_free(mp);
        CHECK(rte_eal_cleanup() == 0);
        return 0;
}
