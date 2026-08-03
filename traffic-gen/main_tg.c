#include "conn_pool.h"
#include "flow.h"
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

#include <rte_byteorder.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_timer.h>

#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>

#define POOL_CAPACITY 1024U

static const uint32_t tg_local_ip = MAKE_IPV4_ADDR(192, 168, 21, 2);

/**
 * Per-packet-worker traffic-generator state.
 *
 * The current runtime has one owner lcore.  When RSS support adds workers,
 * each worker will receive an independent tg_shard with its own flow map,
 * flow pool, scheduler, and statistics.
 */
struct tg_shard {
        struct tg_flow_map flow_map;
        struct tg_conn_pool conn_pool;

        /**
         * This temporary bootstrap flag limits the initial connection test to
         * one attempt.  TODO:The future CPS scheduler replaces this field.
         */
        bool startup_attempted;
};

/**
 * Run per-turn flow scheduling on the socket owner lcore.
 *
 * This bootstrap scheduler starts one hard-coded TCP connection.  A CPS token
 * bucket and concurrency watermark will replace the single-attempt policy.
 */
static void tg_scheduler_tick(void *ctx, unsigned int budget) {
        struct tg_shard *shard = ctx;
        struct sockaddr_in peer = {
            .sin_family = AF_INET,
            .sin_port = rte_cpu_to_be_16(TCP_APP_PORT),
            .sin_addr = {.s_addr = TCP_CLIENT_PEER_IP},
        };

        if (shard == NULL || budget == 0 || shard->startup_attempted)
                return;

        /*
         * Mark before starting so a persistent configuration error does not
         * allocate and close one failed flow on every worker turn.
         */
        shard->startup_attempted = true;

        if (tg_flow_start_tcp(&shard->flow_map, &shard->conn_pool,
                              (const struct sockaddr *)&peer,
                              sizeof(peer)) != 0) {
                LOG_ERROR("traffic-gen TCP startup connect failed: errno=%d",
                          errno);
                return;
        }

        LOG_INFO("traffic-gen TCP connect started: " IP_FMT ":%u",
                 IP_ARG(peer.sin_addr.s_addr), TCP_APP_PORT);
}

/**
 * Deliver one owner-local readiness notification to its registered flow.
 *
 * A failed lookup is normal: a coalesced event may outlive flow teardown, or
 * the socket id may have been reused with a different handle generation.
 */
static void tg_on_event(void *ctx, const struct owner_io_event *event) {
        struct tg_shard *shard = ctx;
        struct tg_flow *flow;

        if (shard == NULL || event == NULL)
                return;

        flow = tg_flow_map_lookup(&shard->flow_map, event->handle);
        if (flow == NULL) {
                LOG_DEBUG("drop stale traffic-gen event: "
                          "sock=%u gen=%u events=0x%x",
                          event->handle.id, event->handle.generation,
                          event->events);
                return;
        }

        LOG_DEBUG("traffic-gen ready sock=%u gen=%u events=0x%x",
                  event->handle.id, event->handle.generation, event->events);

        if (event->events & OWNER_IO_EV_CONNECTED)
                LOG_INFO("traffic-gen TCP connected: sock=%u gen=%u",
                         event->handle.id, event->handle.generation);

        /*
         * tg_flow_on_event() may recycle the object for ERROR or HUP.  The
         * caller must not dereference flow after this invocation.
         */
        tg_flow_on_event(&shard->flow_map, &shard->conn_pool, flow,
                         event->events);
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

        /*
         * Both member initializers clear their own storage, but the bootstrap
         * scheduler flag is owned by this enclosing object and must start
         * false explicitly.
         */
        struct tg_shard shard = {0};
        if (tg_flow_map_init(&shard.flow_map, worker_lcore) != 0)
                rte_exit(EXIT_FAILURE, "traffic-gen flow map init failed\n");

        if (tg_conn_pool_init(&shard.conn_pool, POOL_CAPACITY) != 0)
                rte_exit(EXIT_FAILURE,
                         "traffic-gen connection pool init failed\n");

        struct tg_reactor reactor;
        tg_reactor_init(&reactor, tg_scheduler_tick, tg_on_event, &shard);
        /*
         * stack_runtime_worker_entry() invokes tg_reactor_run() once per
         * owner-worker turn, after RX ingress and TCP timer processing and
         * before transport TX flush.
         */
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
