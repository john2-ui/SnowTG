#include "../tcp-echo/tcp_echo.h"

#include "../../pro-stack/config.h"
#if ENABLE_UDP_APP
#include "../udp-echo/udp_echo.h"
#endif

#include "../../pro-stack/arp.h"
#include "../../pro-stack/ipv4_reassembly.h"
#include "../../pro-stack/net_context.h"
#include "../../pro-stack/port.h"
#include "../../pro-stack/ring.h"
#include "../../pro-stack/socket.h"
#include "../../pro-stack/stack_runtime.h"

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#if ENABLE_PDUMP
#include <rte_pdump.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t demo_local_ip = MAKE_IPV4_ADDR(192, 168, 21, 2);

static int demo_parse_mtu(int argc, char **argv, uint16_t *mtu_out) {
        uint16_t mtu = 0;
        bool seen = false;

        if (mtu_out == NULL)
                return -1;
        for (int i = 1; i < argc; i++) {
                char *end = NULL;
                unsigned long value;

                if (strcmp(argv[i], "--mtu") != 0 || seen || ++i == argc)
                        return -1;
                errno = 0;
                value = strtoul(argv[i], &end, 10);
                if (errno != 0 || end == argv[i] || *end != '\0' ||
                    value < IPV4_MIN_MTU || value > UINT16_MAX)
                        return -1;
                mtu = (uint16_t)value;
                seen = true;
        }
        *mtu_out = mtu;
        return 0;
}

int main(int argc, char *argv[]) {
        int eal_args = rte_eal_init(argc, argv);
        uint16_t requested_mtu;

        if (eal_args < 0)
                rte_exit(EXIT_FAILURE, "rte_eal_init() failed\n");
        argc -= eal_args;
        argv += eal_args;
        if (demo_parse_mtu(argc, argv, &requested_mtu) != 0)
                rte_exit(EXIT_FAILURE, "usage: stack-demo [--mtu BYTES]\n");
        if (socket_registry_init() != 0)
                rte_exit(EXIT_FAILURE, "socket registry init failed\n");
#if ENABLE_PDUMP
        if (rte_pdump_init() < 0)
                rte_exit(EXIT_FAILURE, "rte_pdump_init() failed\n");
#endif

        struct rte_mempool *mp =
            rte_pktmbuf_pool_create("demo_mbuf_pool", NUM_MBUFS, 0, 0,
                                    RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (mp == NULL)
                rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create() failed\n");

        net_context_set_mempool(mp);
        struct port_topology topology = port_init(0, mp, requested_mtu);
        net_context_init(0, demo_local_ip, topology.ipv4_mtu);
        owner_timer_global_init();
        struct ipv4_reassembly reassembly = {0};
        if (ipv4_reassembly_init(&reassembly) != 0)
                rte_exit(EXIT_FAILURE, "IPv4 reassembly init failed\n");

        unsigned int main_lcore = rte_lcore_id();
        unsigned int worker_lcore = rte_get_next_lcore(main_lcore, 1, 0);
        if (worker_lcore == RTE_MAX_LCORE)
                rte_exit(EXIT_FAILURE,
                         "stack demo needs at least two lcores\n");
        if (socket_registry_init_owner(worker_lcore) != 0 ||
            socket_owner_init(worker_lcore) != 0 ||
            ring_init_owner(worker_lcore) != 0 ||
            arp_table_init_owner(worker_lcore) != 0)
                rte_exit(EXIT_FAILURE, "stack owner shard init failed\n");
        struct inout_ring *ring = ring_for_lcore(worker_lcore);
        struct stack_runtime_worker runtime;
        if (stack_runtime_worker_init(&runtime, worker_lcore, 0,
                                      NSOCK_ID_DEFAULT_CAPACITY, mp, ring,
                                      NULL, NULL) != 0)
                rte_exit(EXIT_FAILURE, "stack runtime init failed\n");

        unsigned int next_app_lcore = worker_lcore;
#if ENABLE_UDP_APP
        next_app_lcore = rte_get_next_lcore(next_app_lcore, 1, 0);
        if (next_app_lcore == RTE_MAX_LCORE ||
            rte_eal_remote_launch(udp_echo_entry, mp, next_app_lcore) < 0)
                rte_exit(EXIT_FAILURE, "failed to launch UDP echo app\n");
#endif
#if ENABLE_TCP_APP && ENABLE_TCP_SERVER
        next_app_lcore = rte_get_next_lcore(next_app_lcore, 1, 0);
        if (next_app_lcore == RTE_MAX_LCORE ||
            rte_eal_remote_launch(tcp_echo_server_entry, mp, next_app_lcore) <
                0)
                rte_exit(EXIT_FAILURE, "failed to launch TCP echo server\n");
#endif
#if ENABLE_TCP_APP && ENABLE_TCP_CLIENT
        next_app_lcore = rte_get_next_lcore(next_app_lcore, 1, 0);
        if (next_app_lcore == RTE_MAX_LCORE ||
            rte_eal_remote_launch(tcp_echo_client_entry, mp, next_app_lcore) <
                0)
                rte_exit(EXIT_FAILURE, "failed to launch TCP echo client\n");
#endif
        if (rte_eal_remote_launch(stack_runtime_worker_entry, &runtime,
                                  worker_lcore) < 0)
                rte_exit(EXIT_FAILURE, "failed to launch packet worker\n");

        while (1) {
                struct rte_mbuf *rx[BURST_SIZE];
                unsigned int nb_rx =
                    rte_eth_rx_burst(g_net.port_id, 0, rx, BURST_SIZE);
                unsigned int ready = 0;
                uint64_t now_cycles = rte_get_timer_cycles();
                for (unsigned int i = 0; i < nb_rx; i++) {
                        struct rte_mbuf *mbuf = ipv4_reassembly_process(
                            &reassembly, rx[i], now_cycles);
                        if (mbuf != NULL)
                                rx[ready++] = mbuf;
                }
                unsigned int enq = rte_ring_sp_enqueue_burst(
                    ring->in, (void **)rx, ready, NULL);
                for (unsigned int i = enq; i < ready; i++)
                        rte_pktmbuf_free(rx[i]);
                ipv4_reassembly_maintain(&reassembly, now_cycles);

                struct rte_mbuf *tx[BURST_SIZE];
                unsigned int nb_tx = rte_ring_sc_dequeue_burst(
                    ring->out, (void **)tx, BURST_SIZE, NULL);
                unsigned int sent =
                    rte_eth_tx_burst(g_net.port_id, 0, tx, nb_tx);
                for (unsigned int i = sent; i < nb_tx; i++)
                        rte_pktmbuf_free(tx[i]);
        }
}
