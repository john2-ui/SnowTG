#include "../tcp-echo/tcp_echo.h"

#include "../../pro-stack/config.h"
#if ENABLE_UDP_APP
#include "../udp-echo/udp_echo.h"
#endif

#include "../../pro-stack/arp.h"
#include "../../pro-stack/log.h"
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
#include <rte_timer.h>

static const uint32_t demo_local_ip = MAKE_IPV4_ADDR(192, 168, 21, 2);

#if ENABLE_ARP_SWEEP
static void arp_sweep_cb(__attribute__((unused)) struct rte_timer *timer,
                         void *arg) {
        struct rte_mempool *mp = arg;
        struct inout_ring *ring = ring_instance();

        for (int i = 1; i <= 254; i++) {
                uint32_t dst_ip =
                    (g_net.local_ip & 0x00FFFFFF) | (0xFF000000 & (i << 24));
                uint8_t *known = arp_lookup(dst_ip);
                const uint8_t *dst_mac = known ? known : g_broadcast_mac;
                struct rte_mbuf *arp = arp_build_pkt(
                    mp, RTE_ARP_OP_REQUEST, dst_mac, g_net.local_ip, dst_ip);
                if (arp != NULL)
                        rte_ring_mp_enqueue_burst(ring->out, (void **)&arp, 1,
                                                  NULL);
        }
}
#endif

int main(int argc, char *argv[]) {
        if (rte_eal_init(argc, argv) < 0)
                rte_exit(EXIT_FAILURE, "rte_eal_init() failed\n");
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
        port_init(0, mp);
        net_context_init(0, demo_local_ip);
        struct inout_ring *ring = ring_instance();
        arp_table_instance();
        rte_timer_subsystem_init();

        unsigned int main_lcore = rte_lcore_id();
        unsigned int worker_lcore = rte_get_next_lcore(main_lcore, 1, 0);
        if (worker_lcore == RTE_MAX_LCORE)
                rte_exit(EXIT_FAILURE,
                         "stack demo needs at least two lcores\n");
        if (socket_owner_init(worker_lcore) != 0)
                rte_exit(EXIT_FAILURE, "socket owner init failed\n");

#if ENABLE_ARP_SWEEP
        struct rte_timer arp_timer;
        rte_timer_init(&arp_timer);
        rte_timer_reset(&arp_timer, rte_get_timer_hz() * 60, PERIODICAL,
                        main_lcore, arp_sweep_cb, mp);
#endif

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
        if (rte_eal_remote_launch(stack_runtime_worker_entry, mp,
                                  worker_lcore) < 0)
                rte_exit(EXIT_FAILURE, "failed to launch packet worker\n");

        while (1) {
                struct rte_mbuf *rx[BURST_SIZE];
                unsigned int nb_rx =
                    rte_eth_rx_burst(g_net.port_id, 0, rx, BURST_SIZE);
                unsigned int enq = rte_ring_sp_enqueue_burst(
                    ring->in, (void **)rx, nb_rx, NULL);
                for (unsigned int i = enq; i < nb_rx; i++)
                        rte_pktmbuf_free(rx[i]);

                struct rte_mbuf *tx[BURST_SIZE];
                unsigned int nb_tx = rte_ring_sc_dequeue_burst(
                    ring->out, (void **)tx, BURST_SIZE, NULL);
                unsigned int sent =
                    rte_eth_tx_burst(g_net.port_id, 0, tx, nb_tx);
                for (unsigned int i = sent; i < nb_tx; i++)
                        rte_pktmbuf_free(tx[i]);

                rte_timer_manage();
        }
}
