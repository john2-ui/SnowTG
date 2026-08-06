/**
 * @file main_tg.c
 * @brief Initializes and runs the single-owner DPDK traffic-generator process.
 *
 * Startup compiles the scenario into immutable plan data, provisions one
 * owner-local shard, and connects the reactor to the protocol stack worker.
 * The main lcore continuously transfers NIC RX and TX bursts through stack
 * rings while the owner worker runs TCP, flow, and scheduler work.
 */

#include "core/flow.h"
#include "core/flow_pool.h"
#include "core/reactor.h"
#include "core/scenario.h"
#include "core/scheduler.h"
#include "core/stats.h"

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

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/** @brief Scenario loaded when no application scenario path is supplied. */
#define TG_DEFAULT_SCENARIO_PATH "scenarios/bootstrap_http.json"

/** @brief Source IPv4 address used by the current single-port test topology. */
static const uint32_t tg_local_ip = MAKE_IPV4_ADDR(192, 168, 21, 2);

/**
 * Parse application arguments after EAL has consumed its arguments.
 *
 * The worker count is intentionally validated here even while the runtime is
 * still single-owner.  This reserves the stable CLI shape without allowing
 * an unimplemented worker count to create unpolled NIC queues.
 */
static int tg_parse_app_args(int argc, char *argv[], unsigned int *workers_out,
                             const char **scenario_out) {
        bool workers_seen = false;
        const char *scenario_path = NULL;
        unsigned int workers = 1;

        if (workers_out == NULL || scenario_out == NULL) {
                errno = EINVAL;
                return -1;
        }

        for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "--workers") == 0) {
                        char *end = NULL;
                        unsigned long value;

                        if (workers_seen || ++i == argc) {
                                errno = EINVAL;
                                return -1;
                        }
                        errno = 0;
                        value = strtoul(argv[i], &end, 10);
                        if (errno != 0 || end == argv[i] || *end != '\0' ||
                            value == 0 || value > UINT_MAX) {
                                errno = EINVAL;
                                return -1;
                        }
                        workers = (unsigned int)value;
                        workers_seen = true;
                        continue;
                }
                if (argv[i][0] == '-' || scenario_path != NULL) {
                        errno = EINVAL;
                        return -1;
                }
                scenario_path = argv[i];
        }

        *workers_out = workers;
        *scenario_out =
            scenario_path == NULL ? TG_DEFAULT_SCENARIO_PATH : scenario_path;
        return 0;
}

/**
 * Per-packet-worker traffic-generator state.
 *
 * The current runtime has one owner lcore.  When RSS support adds workers,
 * each worker will receive an independent tg_shard with its own flow map,
 * flow pool, scheduler, and statistics.
 */
struct tg_shard {
        struct tg_flow_map flow_map;
        struct tg_flow_pool flow_pool;
        struct tg_plan plan;
        struct tg_scheduler scheduler;
        struct tg_stats stats;
        struct owner_io_memory_snapshot memory;
        bool scheduling_stop_reported;
        bool duration_stats_reported;
};

/**
 * @brief Prints one cumulative statistics snapshot when scheduling ends.
 *
 * Transactions still in flight are deliberately retained in @p active and are
 * not counted as completed until their flow observer runs.
 */
static void tg_report_duration_stats(const struct tg_shard *shard) {
        uint64_t success_rate_hundredths = 0;
        const struct tg_stats *stats;

        if (shard == NULL)
                return;
        stats = &shard->stats;
        if (stats->txns_done != 0)
                success_rate_hundredths =
                    stats->txns_success * 10000U / stats->txns_done;
        LOG_INFO("traffic-gen duration summary active=%u live_sockets=%u "
                 "started=%" PRIu64 " done=%" PRIu64 " success=%" PRIu64
                 " fail=%" PRIu64 " success_rate=%" PRIu64 ".%02" PRIu64 "%%"
                 " fail_connect=%" PRIu64 " fail_io=%" PRIu64
                 " fail_proto=%" PRIu64 " fail_resource=%" PRIu64
                 " deferred_resource=%" PRIu64 " tx=%" PRIu64 " rx=%" PRIu64,
                 stats->concurrency, shard->scheduler.live_sockets,
                 stats->txns_started, stats->txns_done, stats->txns_success,
                 stats->txns_fail, success_rate_hundredths / 100U,
                 success_rate_hundredths % 100U, stats->fail_connect,
                 stats->fail_io, stats->fail_proto, stats->fail_resource,
                 stats->starts_deferred_resource, stats->bytes_tx,
                 stats->bytes_rx);
}

/**
 * @brief Reconciles scheduler and statistics after one admitted flow ends.
 *
 * This observer is invoked before the flow is reset, so transaction byte
 * counters and protocol identity remain available to the statistics module.
 */
static void tg_on_flow_finished(void *ctx, const struct tg_flow *flow,
                                enum tg_flow_result result) {
        struct tg_shard *shard = ctx;

        if (shard == NULL)
                return;
        tg_scheduler_on_flow_finished(&shard->scheduler);
        tg_stats_on_flow_finished(&shard->stats, flow, result);
}

static void tg_on_socket_created(void *ctx) {
        struct tg_shard *shard = ctx;

        if (shard != NULL)
                tg_scheduler_on_socket_created(&shard->scheduler);
}

static void tg_on_socket_released(void *ctx) {
        struct tg_shard *shard = ctx;

        if (shard != NULL)
                tg_scheduler_on_socket_released(&shard->scheduler);
}

static void tg_drain_tx_ring(struct inout_ring *ring) {
        if (ring == NULL)
                return;

        for (;;) {
                struct rte_mbuf *tx[BURST_SIZE];
                unsigned int nb_tx = rte_ring_sc_dequeue_burst(
                    ring->out, (void **)tx, BURST_SIZE, NULL);
                if (nb_tx == 0)
                        return;

                unsigned int sent =
                    rte_eth_tx_burst(g_net.port_id, 0, tx, nb_tx);
                for (unsigned int i = sent; i < nb_tx; i++)
                        rte_pktmbuf_free(tx[i]);
        }
}

/**
 * @brief Scheduler admission callback that starts one TCP flow for a class.
 *
 * A synchronous setup failure is counted but does not consume an active-flow
 * slot.  The scheduler itself still consumes the CPS token for the attempt.
 */
static int tg_start_class(void *ctx, const struct tg_class_plan *class_plan) {
        struct tg_shard *shard = ctx;

        if (shard == NULL || class_plan == NULL)
                return -1;
        if (tg_flow_start_tcp(
                &shard->flow_map, &shard->flow_pool,
                (const struct sockaddr *)&class_plan->peer,
                sizeof(class_plan->peer), class_plan->proto,
                &class_plan->http_config, class_plan->request_template,
                class_plan->request_template_len, tg_on_flow_finished, shard,
                tg_on_socket_created, tg_on_socket_released, shard) != 0) {
                if (errno == ENOBUFS)
                        tg_stats_on_resource_deferred(&shard->stats);
                else
                        tg_stats_on_start_failure(&shard->stats);
                LOG_ERROR("traffic-gen start failed class=%s errno=%d",
                          class_plan->name, errno);
                return -1;
        }
        tg_stats_on_admitted(&shard->stats);
        return 0;
}

/**
 * @brief Runs bounded plan scheduling and owner-local periodic reporting.
 * @param ctx Pointer to the worker's @ref tg_shard.
 * @param budget Maximum flow-start attempts for this worker turn.
 */
static void tg_shard_tick(void *ctx, unsigned int budget) {
        struct tg_shard *shard = ctx;
        uint64_t now_cycles;

        if (shard == NULL || budget == 0)
                return;
        now_cycles = rte_get_timer_cycles();
        if (owner_io_memory_snapshot(&shard->memory) == 0) {
                bool available = shard->scheduler.resource_paused
                                     ? shard->memory.above_high_water
                                     : !shard->memory.below_low_water;

                tg_scheduler_set_resource_available(&shard->scheduler,
                                                    available);
        }
        (void)tg_scheduler_tick(&shard->scheduler, now_cycles, budget,
                                tg_start_class, shard);
        if (tg_stats_report_due(&shard->stats, now_cycles, rte_get_timer_hz(),
                                shard->plan.report_interval_sec)) {
                LOG_INFO("traffic-gen stats active=%u live_sockets=%u "
                         "started=%" PRIu64 " done=%" PRIu64 " success=%" PRIu64
                         " fail=%" PRIu64 " tx=%" PRIu64 " rx=%" PRIu64,
                         shard->stats.concurrency,
                         shard->scheduler.live_sockets,
                         shard->stats.txns_started, shard->stats.txns_done,
                         shard->stats.txns_success, shard->stats.txns_fail,
                         shard->stats.bytes_tx, shard->stats.bytes_rx);
                LOG_INFO("traffic-gen memory paused=%u pauses=%" PRIu64
                         " tx_avail=%u tx_peak=%u payload_avail=%u "
                         "payload_peak=%u tx_alloc_fail=%u",
                         shard->scheduler.resource_paused,
                         shard->scheduler.resource_pauses,
                         shard->memory.tcp.available[TCP_MEMORY_TX_CHUNK],
                         shard->memory.tcp.peak_in_use[TCP_MEMORY_TX_CHUNK],
                         shard->memory.tcp.available[TCP_MEMORY_PAYLOAD],
                         shard->memory.tcp.peak_in_use[TCP_MEMORY_PAYLOAD],
                         shard->memory.tcp.alloc_fail[TCP_MEMORY_TX_CHUNK]);
        }
        if (tg_scheduler_is_stopped(&shard->scheduler) &&
            !shard->scheduling_stop_reported) {
                shard->scheduling_stop_reported = true;
                LOG_INFO("traffic-gen plan duration elapsed; draining %u flows",
                         shard->stats.concurrency);
        }
        if (tg_scheduler_is_stopped(&shard->scheduler) &&
            !shard->duration_stats_reported) {
                shard->duration_stats_reported = true;
                tg_report_duration_stats(shard);
        }
        if (tg_scheduler_is_stopped(&shard->scheduler) &&
            shard->scheduler.active == 0 && shard->scheduler.live_sockets == 0)
                stack_runtime_request_stop();
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
        tg_flow_on_event(&shard->flow_map, &shard->flow_pool, flow,
                         event->events);
}

/**
 * @brief Initializes DPDK, scenario state, and the owner-worker runtime.
 * @param argc EAL arguments followed by an optional scenario JSON path.
 * @param argv EAL and application argument vector.
 * @return Never returns during normal operation; failures exit through DPDK.
 */
int main(int argc, char *argv[]) {
        const char *scenario_path;
        int eal_args;
        unsigned int worker_count;
        struct tg_shard shard = {0};

        eal_args = rte_eal_init(argc, argv);
        if (eal_args < 0)
                rte_exit(EXIT_FAILURE, "rte_eal_init() failed\n");
        argc -= eal_args;
        argv += eal_args;
        if (tg_parse_app_args(argc, argv, &worker_count, &scenario_path) != 0)
                rte_exit(EXIT_FAILURE,
                         "usage: traffic-gen [--workers N] [scenario.json]\n");
        /*
         * Fail closed until the stack's process-global owner, socket-list,
         * ARP, and I/O-ring state is made per worker.  Proceeding would create
         * multiple owner shards that mutate the same TCB registry and route
         * RX/TX through queue zero, violating both ownership and RSS affinity.
         */
        if (worker_count != 1)
                rte_exit(EXIT_FAILURE,
                         "--workers %u is unavailable: multi-worker stack "
                         "runtime has not been isolated per lcore\n",
                         worker_count);
        if (tg_plan_load_file(&shard.plan, scenario_path) != 0)
                rte_exit(EXIT_FAILURE, "scenario load failed (%s): errno=%d\n",
                         scenario_path, errno);
        if (shard.plan.max_concurrency > NSOCK_ID_MAX)
                rte_exit(EXIT_FAILURE,
                         "scenario max_concurrency exceeds socket capacity\n");
        if (tg_scheduler_init(&shard.scheduler, &shard.plan,
                              rte_get_timer_hz()) != 0)
                rte_exit(EXIT_FAILURE, "scheduler init failed\n");
        tg_stats_init(&shard.stats);
        LOG_INFO("traffic-gen scenario=%s classes=%u cps=%u concurrency=%u",
                 shard.plan.name, shard.plan.class_count, shard.plan.target_cps,
                 shard.plan.max_concurrency);

        if (socket_registry_init() != 0)
                rte_exit(EXIT_FAILURE, "socket registry init failed\n");

        struct rte_mempool *mp =
            rte_pktmbuf_pool_create("tg_mbuf_pool", NUM_MBUFS, 0, 0,
                                    RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (mp == NULL)
                rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create() failed\n");

        net_context_set_mempool(mp);
        port_init_queues(0, mp, (uint16_t)worker_count);
        {
                unsigned int available =
                    mp == NULL ? 0 : rte_mempool_avail_count(mp);
                unsigned int in_use =
                    mp == NULL ? 0 : rte_mempool_in_use_count(mp);

                LOG_INFO(
                    "traffic-gen mbuf pool after port init: mp=%p avail=%u "
                    "in_use=%u",
                    (void *)mp, available, in_use);
        }
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

        if (tg_flow_map_init(&shard.flow_map, worker_lcore) != 0)
                rte_exit(EXIT_FAILURE, "traffic-gen flow map init failed\n");

        if (tg_flow_pool_init(&shard.flow_pool, shard.plan.max_concurrency) !=
            0)
                rte_exit(EXIT_FAILURE, "traffic-gen flow pool init failed\n");

        struct tg_reactor reactor;
        tg_reactor_init(&reactor, tg_shard_tick, tg_on_event, &shard);
        /*
         * stack_runtime_worker_entry() invokes tg_reactor_run() once per
         * owner-worker turn, after RX ingress and TCP timer processing and
         * before transport TX flush.
         */
        stack_runtime_set_reactor(tg_reactor_run, &reactor);
        if (rte_eal_remote_launch(stack_runtime_worker_entry, mp,
                                  worker_lcore) < 0)
                rte_exit(EXIT_FAILURE, "failed to launch owner worker\n");

        while (!stack_runtime_stop_requested()) {
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
        (void)rte_eal_wait_lcore(worker_lcore);
        tg_drain_tx_ring(ring);
        tg_plan_fini(&shard.plan);
        return EXIT_SUCCESS;
}
