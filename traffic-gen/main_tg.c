/**
 * @file main_tg.c
 * @brief Initializes and runs the multi-owner DPDK traffic-generator process.
 *
 * Startup compiles the scenario into immutable plan data, provisions one
 * owner-local shard per flow worker, and connects a reactor to each protocol
 * worker. The main lcore bridges NIC RX/TX queues and SPSC stack rings,
 * software-dispatching RX packets when hardware RSS is unavailable.
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
#include "../pro-stack/rx_dispatch.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/socket_owner.h"
#include "../pro-stack/stack_runtime.h"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_timer.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
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
 * The worker count selects protocol-owner shards. The port setup may use fewer
 * RX queues and software-dispatch packets to those shards when RSS is absent.
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
 * Each owner lcore has an independent flow map, flow pool, scheduler, and
 * statistics. All fields except the drop counters are owner-local.
 */
struct tg_runtime_control {
        atomic_uint remaining_shards;
};

struct tg_shard {
        struct tg_flow_map flow_map;
        struct tg_flow_pool flow_pool;
        struct tg_plan plan;
        struct tg_scheduler scheduler;
        struct tg_stats stats;
        struct owner_io_memory_snapshot memory;
        struct tg_reactor *reactor; /**< Needed to snapshot reactor counters. */
        uint64_t scheduler_starts;  /**< Attempts since the prior report. */
        uint64_t socket_releases; /**< Final TCB releases since prior report. */
        /* The main lcore records dispatch/NIC counters between reports. */
        atomic_uint_fast64_t rx_ring_drops;
        atomic_uint_fast64_t tx_nic_drops;
        atomic_uint_fast64_t rx_owner_hits;
        atomic_uint_fast64_t rx_software_hashes;
        atomic_uint_fast64_t rx_parse_fallbacks;
        struct tg_runtime_control *runtime;
        bool scheduling_enabled;
        bool drained;
        bool scheduling_stop_reported;
        bool duration_stats_reported;
};

/** Complete application and stack context associated with one flow worker. */
struct tg_worker {
        unsigned int lcore_id;
        uint16_t flow_queue_id;
        uint16_t tx_queue_id;
        struct inout_ring *ring;
        struct tg_shard shard;
        struct tg_reactor reactor;
        struct stack_runtime_worker runtime;
};

/** Convert DPDK cycle-clock values for human-readable periodic diagnostics. */
static uint64_t tg_cycles_to_us(uint64_t cycles) {
        uint64_t hz = rte_get_timer_hz();

        return hz == 0 ? 0 : cycles * 1000000U / hz;
}

/** Avoid division by zero for phases not reached in the report interval. */
static uint64_t tg_average_cycles(uint64_t total, uint64_t samples) {
        return samples == 0 ? 0 : total / samples;
}

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

        if (shard != NULL) {
                tg_scheduler_on_socket_released(&shard->scheduler);
                shard->socket_releases++;
        }
}

static void tg_drain_tx_ring(struct inout_ring *ring, uint16_t tx_queue_id) {
        if (ring == NULL)
                return;

        for (;;) {
                struct rte_mbuf *tx[BURST_SIZE];
                unsigned int nb_tx = rte_ring_sc_dequeue_burst(
                    ring->out, (void **)tx, BURST_SIZE, NULL);
                if (nb_tx == 0)
                        return;

                unsigned int sent =
                    rte_eth_tx_burst(g_net.port_id, tx_queue_id, tx, nb_tx);
                for (unsigned int i = sent; i < nb_tx; i++)
                        rte_pktmbuf_free(tx[i]);
        }
}

static bool tg_is_arp_packet(const struct rte_mbuf *mbuf) {
        const struct rte_ether_hdr *eth;

        if (mbuf == NULL || mbuf->pkt_len < sizeof(*eth) ||
            mbuf->data_len < sizeof(*eth))
                return false;
        eth = rte_pktmbuf_mtod(mbuf, const struct rte_ether_hdr *);
        return eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);
}

static void tg_replicate_arp(struct tg_worker *workers,
                             unsigned int worker_count,
                             unsigned int source_index, struct rte_mempool *mp,
                             struct rte_mbuf *mbuf) {
        if (!tg_is_arp_packet(mbuf))
                return;
        mbuf->dynfield1[0] = 0;

        for (unsigned int index = 0; index < worker_count; index++) {
                struct rte_mbuf *clone;

                if (index == source_index)
                        continue;
                clone = rte_pktmbuf_clone(mbuf, mp);
                if (clone == NULL) {
                        atomic_fetch_add(&workers[index].shard.rx_ring_drops,
                                         1);
                        continue;
                }
                clone->dynfield1[0] = ARP_MBUF_F_LEARN_ONLY;
                if (rte_ring_sp_enqueue(workers[index].ring->in, clone) != 0) {
                        rte_pktmbuf_free(clone);
                        atomic_fetch_add(&workers[index].shard.rx_ring_drops,
                                         1);
                }
        }
}

/**
 * Classify a NIC RX burst into owner-local SPSC input rings.
 *
 * The main lcore remains the sole producer of every input ring, preserving
 * the ring flags even when one NIC queue fans out to several workers.
 */
static void tg_dispatch_rx_burst(struct tg_worker *workers,
                                 unsigned int worker_count,
                                 struct rte_mempool *mp, uint16_t rx_queue,
                                 struct rte_mbuf **rx, unsigned int nb_rx) {
        struct rte_mbuf *batches[RTE_MAX_LCORE][BURST_SIZE];
        unsigned int batch_counts[RTE_MAX_LCORE] = {0};

        for (unsigned int packet = 0; packet < nb_rx; packet++) {
                struct rx_dispatch_result result;
                unsigned int target;

                rx_dispatch_classify(rx[packet], rx_queue, &result);
                target = result.worker_index;
                if (target >= worker_count) {
                        target = 0;
                        result.parse_fallback = true;
                }
                if (result.action == RX_DISPATCH_FANOUT)
                        tg_replicate_arp(workers, worker_count, target, mp,
                                         rx[packet]);
                if (result.owner_hit)
                        atomic_fetch_add(&workers[target].shard.rx_owner_hits,
                                         1);
                if (result.software_hash)
                        atomic_fetch_add(
                            &workers[target].shard.rx_software_hashes, 1);
                if (result.parse_fallback)
                        atomic_fetch_add(
                            &workers[target].shard.rx_parse_fallbacks, 1);
                batches[target][batch_counts[target]++] = rx[packet];
        }

        for (unsigned int index = 0; index < worker_count; index++) {
                unsigned int count = batch_counts[index];
                unsigned int enq;

                if (count == 0)
                        continue;
                enq = rte_ring_sp_enqueue_burst(workers[index].ring->in,
                                                (void **)batches[index], count,
                                                NULL);
                for (unsigned int packet = enq; packet < count; packet++)
                        rte_pktmbuf_free(batches[index][packet]);
                if (enq != count)
                        atomic_fetch_add(&workers[index].shard.rx_ring_drops,
                                         count - enq);
        }
}

/**
 * @brief Scheduler admission callback that starts one transport flow.
 *
 * A synchronous setup failure is counted but does not consume an active-flow
 * slot.  The scheduler itself still consumes the CPS token for the attempt.
 */
static int tg_start_class(void *ctx, const struct tg_class_plan *class_plan) {
        struct tg_shard *shard = ctx;
        const void *class_config;
        int start_result;

        if (shard == NULL || class_plan == NULL)
                return -1;
        class_config = class_plan->proto_config;

        if (class_plan->transport == TG_TRANSPORT_TCP) {
                start_result = tg_flow_start_tcp(
                    &shard->flow_map, &shard->flow_pool,
                    (const struct sockaddr *)&class_plan->peer,
                    sizeof(class_plan->peer), class_plan->proto, class_config,
                    class_plan->request_template,
                    class_plan->request_template_len, tg_on_flow_finished,
                    shard, tg_on_socket_created, tg_on_socket_released, shard);
        } else if (class_plan->transport == TG_TRANSPORT_UDP) {
                start_result = tg_flow_start_udp(
                    &shard->flow_map, &shard->flow_pool,
                    (const struct sockaddr *)&class_plan->peer,
                    sizeof(class_plan->peer), class_plan->proto, class_config,
                    class_plan->request_template,
                    class_plan->request_template_len, tg_on_flow_finished,
                    shard, tg_on_socket_created, tg_on_socket_released, shard);
        } else {
                errno = EINVAL;
                start_result = -1;
        }

        if (start_result != 0) {
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
        tg_flow_expire(&shard->flow_map, &shard->flow_pool, now_cycles);
        if (!shard->scheduling_enabled)
                return;
        if (owner_io_memory_snapshot(&shard->memory) == 0) {
                bool available = shard->scheduler.resource_paused
                                     ? shard->memory.above_high_water
                                     : !shard->memory.below_low_water;

                tg_scheduler_set_resource_available(&shard->scheduler,
                                                    available);
        }
        shard->scheduler_starts += tg_scheduler_tick(
            &shard->scheduler, now_cycles, budget, tg_start_class, shard);
        if (tg_stats_report_due(&shard->stats, now_cycles, rte_get_timer_hz(),
                                shard->plan.report_interval_sec)) {
                /*
                 * These counters are interval deltas. Keep flow latency in
                 * tg_stats cumulative so rare long transactions remain visible.
                 */
                struct stack_runtime_metrics runtime = {0};
                uint64_t reactor_turns = 0;
                uint64_t reactor_events = 0;
                uint32_t reactor_burst_high_water = 0;
                uint64_t rx_ring_drops;
                uint64_t tx_nic_drops;
                uint64_t rx_owner_hits;
                uint64_t rx_software_hashes;
                uint64_t rx_parse_fallbacks;

                stack_runtime_metrics_take(&runtime);
                tg_reactor_metrics_take(shard->reactor, &reactor_turns,
                                        &reactor_events,
                                        &reactor_burst_high_water);
                rx_ring_drops = atomic_exchange(&shard->rx_ring_drops, 0);
                tx_nic_drops = atomic_exchange(&shard->tx_nic_drops, 0);
                rx_owner_hits = atomic_exchange(&shard->rx_owner_hits, 0);
                rx_software_hashes =
                    atomic_exchange(&shard->rx_software_hashes, 0);
                rx_parse_fallbacks =
                    atomic_exchange(&shard->rx_parse_fallbacks, 0);
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
                LOG_INFO("traffic-gen worker turns=%" PRIu64 " rx=%" PRIu64
                         " scans=%" PRIu64 " flush=%" PRIu64
                         " dirty_enq=%" PRIu64 " dirty_dedup=%" PRIu64
                         " dirty_requeue=%" PRIu64 " arp_wait=%" PRIu64
                         " arp_wake=%" PRIu64 " dirty_depth=%u dirty_hwm=%u"
                         " budget=%" PRIu64 " turn_avg_us=%" PRIu64
                         " rx_us=%" PRIu64 " maint_us=%" PRIu64
                         " reactor_us=%" PRIu64 " flush_us=%" PRIu64,
                         runtime.worker_turns, runtime.rx_packets,
                         runtime.socket_scans, runtime.tx_flush_calls,
                         runtime.dirty_tx_enqueues, runtime.dirty_tx_dedup_hits,
                         runtime.dirty_tx_requeues, runtime.dirty_tx_arp_waits,
                         runtime.dirty_tx_arp_wakeups, runtime.dirty_tx_depth,
                         runtime.dirty_tx_high_water,
                         runtime.dirty_tx_budget_exhausted,
                         tg_cycles_to_us(tg_average_cycles(
                             runtime.turn_cycles, runtime.worker_turns)),
                         tg_cycles_to_us(runtime.rx_cycles),
                         tg_cycles_to_us(runtime.maintenance_cycles),
                         tg_cycles_to_us(runtime.reactor_cycles),
                         tg_cycles_to_us(runtime.tx_flush_cycles));
                LOG_INFO(
                    "traffic-gen reactor turns=%" PRIu64 " events=%" PRIu64
                    " event_burst_hwm=%u starts=%" PRIu64 " tokens=%" PRIu64
                    " socket_releases=%" PRIu64
                    " ring_hwm_in=%u ring_hwm_out=%u"
                    " rx_ring_drops=%" PRIu64 " tx_nic_drops=%" PRIu64
                    " rx_owner_hits=%" PRIu64 " rx_software_hashes=%" PRIu64
                    " rx_parse_fallbacks=%" PRIu64,
                    reactor_turns, reactor_events, reactor_burst_high_water,
                    shard->scheduler_starts,
                    shard->scheduler.token_numerator /
                        shard->scheduler.cycles_per_second,
                    shard->socket_releases, runtime.in_ring_high_water,
                    runtime.out_ring_high_water, rx_ring_drops, tx_nic_drops,
                    rx_owner_hits, rx_software_hashes, rx_parse_fallbacks);
                LOG_INFO("traffic-gen latency connect_avg_us=%" PRIu64
                         " connect_max_us=%" PRIu64 " first_rx_avg_us=%" PRIu64
                         " first_rx_max_us=%" PRIu64 " complete_avg_us=%" PRIu64
                         " complete_max_us=%" PRIu64,
                         tg_cycles_to_us(
                             tg_average_cycles(shard->stats.connect_cycles,
                                               shard->stats.connect_samples)),
                         tg_cycles_to_us(shard->stats.connect_max_cycles),
                         tg_cycles_to_us(
                             tg_average_cycles(shard->stats.first_rx_cycles,
                                               shard->stats.first_rx_samples)),
                         tg_cycles_to_us(shard->stats.first_rx_max_cycles),
                         tg_cycles_to_us(
                             tg_average_cycles(shard->stats.complete_cycles,
                                               shard->stats.complete_samples)),
                         tg_cycles_to_us(shard->stats.complete_max_cycles));
                shard->scheduler_starts = 0;
                shard->socket_releases = 0;
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
            shard->scheduler.active == 0 &&
            shard->scheduler.live_sockets == 0 && !shard->drained) {
                shard->drained = true;
                if (shard->runtime != NULL &&
                    atomic_fetch_sub(&shard->runtime->remaining_shards, 1) == 1)
                        stack_runtime_request_stop();
        }
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

        // if (event->events & OWNER_IO_EV_CONNECTED)
        //         LOG_INFO("traffic-gen TCP connected: sock=%u gen=%u",
        //                  event->handle.id, event->handle.generation);

        /*
         * tg_flow_on_event() may recycle the object for ERROR or HUP.  The
         * caller must not dereference flow after this invocation.
         */
        tg_flow_on_event(&shard->flow_map, &shard->flow_pool, flow,
                         event->events);
}

static void tg_report_aggregate(const struct tg_worker *workers,
                                unsigned int worker_count) {
        uint64_t started = 0;
        uint64_t done = 0;
        uint64_t success = 0;
        uint64_t failed = 0;
        uint64_t bytes_tx = 0;
        uint64_t bytes_rx = 0;

        for (unsigned int index = 0; index < worker_count; index++) {
                const struct tg_stats *stats = &workers[index].shard.stats;

                started += stats->txns_started;
                done += stats->txns_done;
                success += stats->txns_success;
                failed += stats->txns_fail;
                bytes_tx += stats->bytes_tx;
                bytes_rx += stats->bytes_rx;
        }
        LOG_INFO(
            "traffic-gen aggregate workers=%u started=%" PRIu64 " done=%" PRIu64
            " success=%" PRIu64 " fail=%" PRIu64 " tx=%" PRIu64 " rx=%" PRIu64,
            worker_count, started, done, success, failed, bytes_tx, bytes_rx);
}

/*
 * ============================================================================
 * traffic-gen main() execution flow
 *
 *   +-------+
 *   | Start |
 *   +---+---+
 *       |
 *       v
 *   +---+---------------------------+
 *   | Initialize DPDK EAL          |
 *   +---+---------------------------+
 *       |
 *       v
 *   +---+---------------------------+
 *   | Parse arguments and load the  |
 *   | scenario plan                 |
 *   +---+---------------------------+
 *       |
 *       v
 *   +---+---------------------------+
 *   | Initialize global networking |
 *   | resources and runtime state  |
 *   +---+---------------------------+
 *       |
 *       v
 *   +---+---------------------------+
 *   | Initialize every worker:     |
 *   | shard, reactor, and runtime  |
 *   +---+---------------------------+
 *       |
 *       v
 *   +---+---------------------------+
 *   | Configure RX dispatch and    |
 *   | launch worker lcores         |
 *   +---+---------------------------+
 *       |
 *       v
 *   +---+---------------------------+
 *   | Main loop: receive/dispatch  |
 *   | RX packets and transmit TX  |
 *   +---+---------------------------+
 *       |
 *       v
 *   +---+---------------------------+
 *   | Stop requested?              |
 *   +--+------------------------+--+
 *      | No                     | Yes
 *      |                        v
 *      +------------------+  +--+---------------------------+
 *                         |  | Wait, drain, report, and    |
 *                         +->| release resources           |
 *                            +--+---------------------------+
 *                                |
 *                                v
 *                            +---+---+
 *                            | Exit  |
 *                            +-------+
 *
 * Initialization failures follow DPDK's EXIT_FAILURE path.
 * ============================================================================
 */
/**
 * @brief Initializes DPDK, scenario state, and the owner-worker runtime.
 * @param argc EAL arguments followed by an optional scenario JSON path.
 * @param argv EAL and application argument vector.
 * @return EXIT_SUCCESS after clean shutdown; failures exit through DPDK.
 */
int main(int argc, char *argv[]) {
        const char *scenario_path;
        int eal_args;
        unsigned int worker_count;
        unsigned int main_lcore;
        unsigned int active_shards;
        struct tg_plan plan = {0};
        struct tg_worker *workers;
        struct tg_runtime_control runtime = {0};
        struct rte_mempool *mp;
        struct port_topology port_topology;

        /* Stage 1: Initialize DPDK and separate EAL arguments from app args. */
        eal_args = rte_eal_init(argc, argv);
        if (eal_args < 0)
                rte_exit(EXIT_FAILURE, "rte_eal_init() failed\n");
        argc -= eal_args;
        argv += eal_args;

        /* Stage 2: Parse application options and locate the scenario file. */
        if (tg_parse_app_args(argc, argv, &worker_count, &scenario_path) != 0)
                rte_exit(EXIT_FAILURE,
                         "usage: traffic-gen [--workers N] [scenario.json]\n");

        /* Stage 3: Load the scenario and validate its shard constraints. */
        if (worker_count > RTE_MAX_LCORE)
                rte_exit(EXIT_FAILURE, "--workers %u exceeds lcore capacity\n",
                         worker_count);
        if (tg_plan_load_file(&plan, scenario_path) != 0)
                rte_exit(EXIT_FAILURE, "scenario load failed (%s): errno=%d\n",
                         scenario_path, errno);
        active_shards = tg_plan_active_shards(&plan, worker_count);
        if (active_shards == 0)
                rte_exit(EXIT_FAILURE,
                         "scenario has no active scheduling shard\n");
        if (((uint64_t)plan.max_concurrency + active_shards - 1U) /
                active_shards >
            NSOCK_ID_MAX)
                rte_exit(EXIT_FAILURE,
                         "scenario concurrency exceeds per-shard socket "
                         "capacity for %u active shards\n",
                         active_shards);
        LOG_INFO(
            "traffic-gen scenario=%s classes=%u workers=%u active_shards=%u "
            "cps=%u concurrency=%u",
            plan.name, plan.class_count, worker_count, active_shards,
            plan.target_cps, plan.max_concurrency);

        /* Stage 4: Initialize global packet, network, and timer resources. */
        if (socket_registry_init() != 0)
                rte_exit(EXIT_FAILURE, "socket registry init failed\n");

        mp =
            rte_pktmbuf_pool_create("tg_mbuf_pool", NUM_MBUFS, 0, 0,
                                    RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (mp == NULL)
                rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create() failed\n");

        net_context_set_mempool(mp);
        port_topology = port_init_queues(0, mp, (uint16_t)worker_count);
        net_context_init(0, tg_local_ip);
        rte_timer_subsystem_init();

        /* Stage 5: Allocate worker contexts and initialize shared runtime
         * state. */
        main_lcore = rte_lcore_id();
        workers = calloc(worker_count, sizeof(*workers));
        if (workers == NULL)
                rte_exit(EXIT_FAILURE, "worker context allocation failed\n");

        atomic_init(&runtime.remaining_shards, active_shards);

        /*
         * Stage 6: Initialize each worker's lcore assignment, ownership
         * resources, flow state, scheduler, reactor, and stack runtime.
         */
        unsigned int previous_lcore = main_lcore;
        for (unsigned int index = 0; index < worker_count; index++) {
                struct tg_worker *worker = &workers[index];

                /* Assign a worker lcore and map its packet queues. */
                worker->lcore_id = rte_get_next_lcore(previous_lcore, 1, 0);
                if (worker->lcore_id == RTE_MAX_LCORE)
                        rte_exit(EXIT_FAILURE,
                                 "traffic-gen needs main plus %u worker "
                                 "lcores\n",
                                 worker_count);
                previous_lcore = worker->lcore_id;
                worker->flow_queue_id = (uint16_t)index;
                worker->tx_queue_id =
                    (uint16_t)(index % port_topology.tx_queue_count);
                worker->shard.runtime = &runtime;
                worker->shard.scheduling_enabled = index < active_shards;
                tg_stats_init(&worker->shard.stats);

                /* Initialize resources owned by this worker's lcore. */
                if (socket_registry_init_owner(worker->lcore_id) != 0 ||
                    socket_owner_init(worker->lcore_id) != 0 ||
                    ring_init_owner(worker->lcore_id) != 0 ||
                    arp_table_init_owner(worker->lcore_id) != 0)
                        rte_exit(EXIT_FAILURE,
                                 "worker %u shard initialization failed\n",
                                 index);
                worker->ring = ring_for_lcore(worker->lcore_id);
                if (tg_flow_map_init(&worker->shard.flow_map,
                                     worker->lcore_id) != 0)
                        rte_exit(EXIT_FAILURE, "worker %u flow map failed\n",
                                 index);

                /* Build the scheduler and flow pool for active shards only. */
                if (worker->shard.scheduling_enabled) {
                        if (tg_plan_partition(&worker->shard.plan, &plan, index,
                                              active_shards) != 0 ||
                            tg_scheduler_init(&worker->shard.scheduler,
                                              &worker->shard.plan,
                                              rte_get_timer_hz()) != 0 ||
                            tg_flow_pool_init(
                                &worker->shard.flow_pool,
                                worker->shard.plan.max_concurrency) != 0)
                                rte_exit(EXIT_FAILURE,
                                         "worker %u scheduler initialization "
                                         "failed\n",
                                         index);
                }

                /* Attach the reactor and start-ready stack runtime context. */
                tg_reactor_init(&worker->reactor, tg_shard_tick, tg_on_event,
                                &worker->shard);
                worker->shard.reactor = &worker->reactor;
                if (stack_runtime_worker_init(
                        &worker->runtime, worker->lcore_id,
                        worker->flow_queue_id, mp, worker->ring, tg_reactor_run,
                        &worker->reactor) != 0)
                        rte_exit(EXIT_FAILURE,
                                 "worker %u runtime initialization failed\n",
                                 index);
        }

        /* Stage 7: Configure RX dispatch with the worker lcore assignments. */
        {
                unsigned int worker_lcores[RTE_MAX_LCORE];

                for (unsigned int index = 0; index < worker_count; index++)
                        worker_lcores[index] = workers[index].lcore_id;
                if (rx_dispatch_configure_workers(worker_lcores,
                                                  (uint16_t)worker_count) != 0)
                        rte_exit(EXIT_FAILURE,
                                 "traffic-gen RX dispatcher initialization "
                                 "failed\n");
        }

        /* Stage 8: Launch one stack runtime on each worker lcore. */
        for (unsigned int index = 0; index < worker_count; index++) {
                if (rte_eal_remote_launch(stack_runtime_worker_entry,
                                          &workers[index].runtime,
                                          workers[index].lcore_id) < 0)
                        rte_exit(EXIT_FAILURE,
                                 "failed to launch worker %u on lcore %u\n",
                                 index, workers[index].lcore_id);
        }

        /*
         * Stage 9: Poll NIC RX, dispatch packets to workers, and transmit
         * worker output until the runtime requests shutdown.
         */
        while (!stack_runtime_stop_requested()) {
                /* Receive bursts from every RX queue and dispatch them. */
                for (uint16_t rx_queue = 0;
                     rx_queue < port_topology.rx_queue_count; rx_queue++) {
                        struct rte_mbuf *rx[BURST_SIZE];
                        unsigned int nb_rx = rte_eth_rx_burst(
                            g_net.port_id, rx_queue, rx, BURST_SIZE);

                        if (nb_rx != 0)
                                tg_dispatch_rx_burst(workers, worker_count, mp,
                                                     rx_queue, rx, nb_rx);
                }

                /* Drain each worker's TX ring and account for NIC drops. */
                for (unsigned int index = 0; index < worker_count; index++) {
                        struct tg_worker *worker = &workers[index];
                        struct rte_mbuf *tx[BURST_SIZE];
                        unsigned int nb_tx = rte_ring_sc_dequeue_burst(
                            worker->ring->out, (void **)tx, BURST_SIZE, NULL);
                        if (nb_tx != 0) {
                                unsigned int sent = rte_eth_tx_burst(
                                    g_net.port_id, worker->tx_queue_id, tx,
                                    nb_tx);
                                for (unsigned int i = sent; i < nb_tx; i++)
                                        rte_pktmbuf_free(tx[i]);
                                if (sent != nb_tx)
                                        atomic_fetch_add(
                                            &worker->shard.tx_nic_drops,
                                            nb_tx - sent);
                        }
                }
        }

        /* Stage 10: Wait for workers to stop and flush pending TX packets. */
        for (unsigned int index = 0; index < worker_count; index++)
                (void)rte_eal_wait_lcore(workers[index].lcore_id);
        for (unsigned int index = 0; index < worker_count; index++)
                tg_drain_tx_ring(workers[index].ring,
                                 workers[index].tx_queue_id);

        /* Stage 11: Report aggregate traffic statistics from all workers. */
        tg_report_aggregate(workers, worker_count);

        /* Stage 12: Release per-worker and global resources before exit. */
        for (unsigned int index = 0; index < worker_count; index++) {
                tg_flow_pool_fini(&workers[index].shard.flow_pool);
                tg_flow_map_fini(&workers[index].shard.flow_map);
                tg_plan_fini(&workers[index].shard.plan);
        }
        socket_owner_fini();
        arp_table_fini();
        ring_fini();
        rx_dispatch_reset();
        socket_registry_fini();
        tg_plan_fini(&plan);
        free(workers);
        return EXIT_SUCCESS;
}
