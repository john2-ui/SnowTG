#include "reactor.h"

#include "../pro-stack/arp.h"
#include "../pro-stack/config.h"
#include "../pro-stack/log.h"
#include "../pro-stack/net_context.h"
#include "../pro-stack/port.h"
#include "../pro-stack/ring.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/socket_owner.h"
#include "../pro-stack/stack_runtime.h"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_timer.h>

#include <stdlib.h>

static const uint32_t tg_local_ip = MAKE_IPV4_ADDR(192, 168, 21, 2);

static void tg_on_event(void *ctx, const struct owner_io_event *event) {
        (void)ctx;
        LOG_DEBUG("traffic-gen ready sock=%u gen=%u events=0x%x",
                  event->handle.id, event->handle.generation, event->events);
}

int main(int argc, char *argv[]) {
        if (rte_eal_init(argc, argv) < 0)
                rte_exit(EXIT_FAILURE, "rte_eal_init() failed\n");
        if (socket_registry_init() != 0)
                rte_exit(EXIT_FAILURE, "socket registry init failed\n");

        struct rte_mempool *mp =
            rte_pktmbuf_pool_create("tg_mbuf_pool", NUM_MBUFS, 0, 0,
                                    RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (mp == NULL)
                rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create() failed\n");

        net_context_set_mempool(mp);
        port_init(0, mp);
        net_context_init(0, tg_local_ip);
        struct inout_ring *ring = ring_instance();
        arp_table_instance();
        rte_timer_subsystem_init();

        unsigned int main_lcore = rte_lcore_id();
        unsigned int worker_lcore = rte_get_next_lcore(main_lcore, 1, 0);
        if (worker_lcore == RTE_MAX_LCORE)
                rte_exit(EXIT_FAILURE,
                         "traffic-gen needs at least two lcores\n");
        if (socket_owner_init(worker_lcore) != 0)
                rte_exit(EXIT_FAILURE, "socket owner init failed\n");

        struct tg_reactor reactor;
        tg_reactor_init(&reactor, tg_on_event, NULL);
        stack_runtime_set_reactor(tg_reactor_run, &reactor);
        if (rte_eal_remote_launch(stack_runtime_worker_entry, mp,
                                  worker_lcore) < 0)
                rte_exit(EXIT_FAILURE, "failed to launch owner worker\n");

        while (1) {
                struct rte_mbuf *rx[BURST_SIZE];
                unsigned int nb_rx =
                    rte_eth_rx_burst(g_net.port_id, 0, rx, BURST_SIZE);
                if (nb_rx != 0) {
                        unsigned int enq = rte_ring_sp_enqueue_burst(
                            ring->in, (void **)rx, nb_rx, NULL);
                        for (unsigned int i = enq; i < nb_rx; i++)
                                rte_pktmbuf_free(rx[i]);
                }

                struct rte_mbuf *tx[BURST_SIZE];
                unsigned int nb_tx = rte_ring_sc_dequeue_burst(
                    ring->out, (void **)tx, BURST_SIZE, NULL);
                if (nb_tx != 0) {
                        unsigned int sent =
                            rte_eth_tx_burst(g_net.port_id, 0, tx, nb_tx);
                        for (unsigned int i = sent; i < nb_tx; i++)
                                rte_pktmbuf_free(tx[i]);
                }
        }
}
