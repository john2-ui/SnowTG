/**
 * @file stats.c
 * @brief Implements owner-local cumulative transaction accounting.
 *
 * Completion accounting runs before a flow is reset and returned to its pool,
 * allowing the request and response byte counters to be captured exactly
 * once.  No atomic operations are required because a shard has one writer.
 */

#include "stats.h"

#include <rte_cycles.h>
#include <string.h>

/** @copydoc tg_stats_init */
void tg_stats_init(struct tg_stats *stats) {
        if (stats != NULL)
                memset(stats, 0, sizeof(*stats));
}

/** @copydoc tg_stats_on_admitted */
void tg_stats_on_admitted(struct tg_stats *stats) {
        if (stats == NULL)
                return;
        stats->txns_started++;
        stats->concurrency++;
}

void tg_stats_on_connection_created(struct tg_stats *stats) {
        if (stats != NULL)
                stats->connections_created++;
}

void tg_stats_on_connection_reused(struct tg_stats *stats) {
        if (stats != NULL)
                stats->connections_reused++;
}

/** @copydoc tg_stats_on_start_failure */
void tg_stats_on_start_failure(struct tg_stats *stats) {
        if (stats == NULL)
                return;
        stats->txns_started++;
        stats->txns_done++;
        stats->txns_fail++;
        stats->fail_connect++;
}

/** @copydoc tg_stats_on_resource_deferred */
void tg_stats_on_resource_deferred(struct tg_stats *stats) {
        if (stats != NULL)
                stats->starts_deferred_resource++;
}

/** @copydoc tg_stats_on_flow_finished */
void tg_stats_on_flow_finished(struct tg_stats *stats,
                               const struct tg_flow *flow,
                               enum tg_flow_result result) {
        uint64_t now;

        if (stats == NULL || flow == NULL)
                return;

        now = rte_get_timer_cycles();
        if (flow->start_cycles != 0 && now >= flow->start_cycles) {
                uint64_t elapsed = now - flow->start_cycles;

                stats->complete_samples++;
                stats->complete_cycles += elapsed;
                if (elapsed > stats->complete_max_cycles)
                        stats->complete_max_cycles = elapsed;
        }
        if (flow->connected_cycles >= flow->start_cycles &&
            flow->start_cycles != 0) {
                uint64_t elapsed = flow->connected_cycles - flow->start_cycles;

                stats->connect_samples++;
                stats->connect_cycles += elapsed;
                if (elapsed > stats->connect_max_cycles)
                        stats->connect_max_cycles = elapsed;
        }
        if (flow->first_rx_cycles >= flow->start_cycles &&
            flow->start_cycles != 0) {
                uint64_t elapsed = flow->first_rx_cycles - flow->start_cycles;

                stats->first_rx_samples++;
                stats->first_rx_cycles += elapsed;
                if (elapsed > stats->first_rx_max_cycles)
                        stats->first_rx_max_cycles = elapsed;
        }
        stats->txns_done++;
        if (stats->concurrency != 0)
                stats->concurrency--;
        stats->bytes_tx += flow->txn.request_offset;
        stats->bytes_rx += flow->txn.response_bytes;
        if (result == TG_FLOW_RESULT_SUCCESS) {
                stats->txns_success++;
                if (flow->txn.proto != NULL &&
                    strcmp(flow->txn.proto->name, "http") == 0)
                        stats->http_rps_total++;
                return;
        }

        stats->txns_fail++;
        switch (result) {
        case TG_FLOW_RESULT_CONNECT_FAILURE:
                stats->fail_connect++;
                break;
        case TG_FLOW_RESULT_PROTOCOL_FAILURE:
                stats->fail_proto++;
                break;
        case TG_FLOW_RESULT_RESOURCE_PRESSURE:
                stats->fail_resource++;
                break;
        case TG_FLOW_RESULT_IO_FAILURE:
        default:
                stats->fail_io++;
                break;
        }
}

/** Fields summed when a runtime snapshot is accumulated. */
#define TG_RUNTIME_SUM_FIELDS(X)                                               \
        X(memory_paused)                                                       \
        X(memory_pauses)                                                       \
        X(tx_available)                                                        \
        X(payload_available)                                                   \
        X(tx_alloc_fail)                                                       \
        X(worker_turns)                                                        \
        X(rx_packets)                                                          \
        X(socket_scans)                                                        \
        X(tx_flush_calls)                                                      \
        X(dirty_tx_enqueues)                                                   \
        X(dirty_tx_dedup_hits)                                                 \
        X(dirty_tx_requeues)                                                   \
        X(dirty_tx_arp_waits)                                                  \
        X(dirty_tx_arp_wakeups)                                                \
        X(dirty_tx_budget_exhausted)                                           \
        X(turn_cycles)                                                         \
        X(rx_cycles)                                                           \
        X(maintenance_cycles)                                                  \
        X(reactor_cycles)                                                      \
        X(tx_flush_cycles)                                                     \
        X(reactor_turns)                                                       \
        X(reactor_events)                                                      \
        X(scheduler_starts)                                                    \
        X(tokens)                                                              \
        X(socket_releases)                                                     \
        X(rx_ring_drops)                                                       \
        X(tx_nic_drops)                                                        \
        X(udp_tx_queue_drops)                                                  \
        X(rx_owner_hits)                                                       \
        X(rx_software_hashes)                                                  \
        X(rx_parse_fallbacks)                                                  \
        X(stats_queue_drops)                                                   \
        X(ofo_accepted_segments)                                               \
        X(ofo_accepted_bytes)                                                  \
        X(ofo_released_segments)                                               \
        X(ofo_released_bytes)                                                  \
        X(ofo_drop_rcvbuf)                                                     \
        X(ofo_drop_seg_limit)                                                  \
        X(ofo_drop_byte_limit)                                                 \
        X(ofo_drop_owner_limit)                                                \
        X(ofo_drop_alloc)                                                      \
        X(ofo_drop_pressure)                                                   \
        X(ofo_pressure_transitions)

/** Fields for which an aggregate should retain the largest observation. */
#define TG_RUNTIME_MAX_FIELDS(X)                                               \
        X(tx_peak)                                                             \
        X(payload_peak)                                                        \
        X(dirty_tx_depth)                                                      \
        X(dirty_tx_high_water)                                                 \
        X(reactor_burst_high_water)                                            \
        X(ring_hwm_in)                                                         \
        X(ring_hwm_out)                                                        \
        X(ofo_segments_peak)                                                   \
        X(ofo_bytes_peak)                                                      \
        X(ofo_reorder_distance_max)

/** Owner gauges: latest within a worker interval, summed across workers. */
#define TG_RUNTIME_GAUGE_FIELDS(X)                                             \
        X(ofo_segments_current)                                                \
        X(ofo_bytes_current)                                                   \
        X(ofo_pressure_active)

/** @copydoc tg_stats_snapshot_from_stats */
void tg_stats_snapshot_from_stats(struct tg_stats_snapshot *snapshot,
                                  const struct tg_stats *stats,
                                  uint64_t timestamp_cycles, uint64_t sequence,
                                  uint64_t worker_index, uint64_t lcore_id,
                                  enum tg_stats_snapshot_phase phase) {
        if (snapshot == NULL)
                return;
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->timestamp_cycles = timestamp_cycles;
        snapshot->sequence = sequence;
        snapshot->worker_index = worker_index;
        snapshot->lcore_id = lcore_id;
        snapshot->phase = phase;
        if (stats == NULL)
                return;

        snapshot->txns_started = stats->txns_started;
        snapshot->txns_done = stats->txns_done;
        snapshot->txns_success = stats->txns_success;
        snapshot->txns_fail = stats->txns_fail;
        snapshot->fail_connect = stats->fail_connect;
        snapshot->fail_io = stats->fail_io;
        snapshot->fail_proto = stats->fail_proto;
        snapshot->fail_resource = stats->fail_resource;
        snapshot->starts_deferred_resource = stats->starts_deferred_resource;
        snapshot->bytes_tx = stats->bytes_tx;
        snapshot->bytes_rx = stats->bytes_rx;
        snapshot->http_rps_total = stats->http_rps_total;
        snapshot->concurrency = stats->concurrency;
        snapshot->connections_created = stats->connections_created;
        snapshot->connections_reused = stats->connections_reused;
        snapshot->connect_samples = stats->connect_samples;
        snapshot->connect_cycles = stats->connect_cycles;
        snapshot->connect_max_cycles = stats->connect_max_cycles;
        snapshot->first_rx_samples = stats->first_rx_samples;
        snapshot->first_rx_cycles = stats->first_rx_cycles;
        snapshot->first_rx_max_cycles = stats->first_rx_max_cycles;
        snapshot->complete_samples = stats->complete_samples;
        snapshot->complete_cycles = stats->complete_cycles;
        snapshot->complete_max_cycles = stats->complete_max_cycles;
}

/** @copydoc tg_stats_snapshot_add_runtime */
void tg_stats_snapshot_add_runtime(struct tg_stats_snapshot *total,
                                   const struct tg_stats_snapshot *sample) {
        if (total == NULL || sample == NULL)
                return;

#define TG_ADD_FIELD(field) total->field += sample->field;
        TG_RUNTIME_SUM_FIELDS(TG_ADD_FIELD)
#undef TG_ADD_FIELD
#define TG_MAX_FIELD(field)                                                    \
        if (sample->field > total->field)                                      \
                total->field = sample->field;
        TG_RUNTIME_MAX_FIELDS(TG_MAX_FIELD)
#undef TG_MAX_FIELD
#define TG_LATEST_FIELD(field) total->field = sample->field;
        TG_RUNTIME_GAUGE_FIELDS(TG_LATEST_FIELD)
#undef TG_LATEST_FIELD
}

/** @copydoc tg_stats_snapshot_copy_runtime */
void tg_stats_snapshot_copy_runtime(struct tg_stats_snapshot *snapshot,
                                    const struct tg_stats_snapshot *total) {
        if (snapshot == NULL || total == NULL)
                return;

#define TG_COPY_FIELD(field) snapshot->field = total->field;
        TG_RUNTIME_SUM_FIELDS(TG_COPY_FIELD)
        TG_RUNTIME_MAX_FIELDS(TG_COPY_FIELD)
        TG_RUNTIME_GAUGE_FIELDS(TG_COPY_FIELD)
#undef TG_COPY_FIELD
}

/** @copydoc tg_stats_snapshot_add */
void tg_stats_snapshot_add(struct tg_stats_snapshot *aggregate,
                           const struct tg_stats_snapshot *sample) {
        if (aggregate == NULL || sample == NULL)
                return;
        if (sample->timestamp_cycles > aggregate->timestamp_cycles)
                aggregate->timestamp_cycles = sample->timestamp_cycles;
        if (sample->sequence > aggregate->sequence)
                aggregate->sequence = sample->sequence;
        aggregate->txns_started += sample->txns_started;
        aggregate->txns_done += sample->txns_done;
        aggregate->txns_success += sample->txns_success;
        aggregate->txns_fail += sample->txns_fail;
        aggregate->fail_connect += sample->fail_connect;
        aggregate->fail_io += sample->fail_io;
        aggregate->fail_proto += sample->fail_proto;
        aggregate->fail_resource += sample->fail_resource;
        aggregate->starts_deferred_resource += sample->starts_deferred_resource;
        aggregate->bytes_tx += sample->bytes_tx;
        aggregate->bytes_rx += sample->bytes_rx;
        aggregate->http_rps_total += sample->http_rps_total;
        aggregate->concurrency += sample->concurrency;
        aggregate->live_sockets += sample->live_sockets;
        aggregate->connections_created += sample->connections_created;
        aggregate->connections_reused += sample->connections_reused;
        aggregate->connect_samples += sample->connect_samples;
        aggregate->connect_cycles += sample->connect_cycles;
        if (sample->connect_max_cycles > aggregate->connect_max_cycles)
                aggregate->connect_max_cycles = sample->connect_max_cycles;
        aggregate->first_rx_samples += sample->first_rx_samples;
        aggregate->first_rx_cycles += sample->first_rx_cycles;
        if (sample->first_rx_max_cycles > aggregate->first_rx_max_cycles)
                aggregate->first_rx_max_cycles = sample->first_rx_max_cycles;
        aggregate->complete_samples += sample->complete_samples;
        aggregate->complete_cycles += sample->complete_cycles;
        if (sample->complete_max_cycles > aggregate->complete_max_cycles)
                aggregate->complete_max_cycles = sample->complete_max_cycles;
#define TG_ADD_FIELD(field) aggregate->field += sample->field;
        TG_RUNTIME_SUM_FIELDS(TG_ADD_FIELD)
        TG_RUNTIME_GAUGE_FIELDS(TG_ADD_FIELD)
#undef TG_ADD_FIELD
#define TG_MAX_FIELD(field)                                                    \
        if (sample->field > aggregate->field)                                  \
                aggregate->field = sample->field;
        TG_RUNTIME_MAX_FIELDS(TG_MAX_FIELD)
#undef TG_MAX_FIELD
}

/** @copydoc tg_stats_channel_init */
void tg_stats_channel_init(struct tg_stats_channel *channel) {
        if (channel == NULL)
                return;
        memset(channel->records, 0, sizeof(channel->records));
        atomic_init(&channel->write_seq, 0);
        atomic_init(&channel->read_seq, 0);
        atomic_init(&channel->dropped, 0);
}

/** @copydoc tg_stats_channel_publish */
bool tg_stats_channel_publish(struct tg_stats_channel *channel,
                              const struct tg_stats_snapshot *snapshot) {
        uint64_t write_seq;
        uint64_t read_seq;

        if (channel == NULL || snapshot == NULL)
                return false;
        write_seq =
            atomic_load_explicit(&channel->write_seq, memory_order_relaxed);
        read_seq =
            atomic_load_explicit(&channel->read_seq, memory_order_acquire);
        if (write_seq - read_seq >= TG_STATS_CHANNEL_CAP) {
                atomic_fetch_add_explicit(&channel->dropped, 1,
                                          memory_order_relaxed);
                return false;
        }
        channel->records[write_seq % TG_STATS_CHANNEL_CAP] = *snapshot;
        atomic_store_explicit(&channel->write_seq, write_seq + 1,
                              memory_order_release);
        return true;
}

/** @copydoc tg_stats_channel_consume */
bool tg_stats_channel_consume(struct tg_stats_channel *channel,
                              struct tg_stats_snapshot *snapshot) {
        uint64_t write_seq;
        uint64_t read_seq;

        if (channel == NULL || snapshot == NULL)
                return false;
        read_seq =
            atomic_load_explicit(&channel->read_seq, memory_order_relaxed);
        write_seq =
            atomic_load_explicit(&channel->write_seq, memory_order_acquire);
        if (read_seq == write_seq)
                return false;
        *snapshot = channel->records[read_seq % TG_STATS_CHANNEL_CAP];
        atomic_store_explicit(&channel->read_seq, read_seq + 1,
                              memory_order_release);
        return true;
}

/** @copydoc tg_stats_channel_take_dropped */
uint64_t tg_stats_channel_take_dropped(struct tg_stats_channel *channel) {
        if (channel == NULL)
                return 0;
        return atomic_exchange_explicit(&channel->dropped, 0,
                                        memory_order_acq_rel);
}

/** @copydoc tg_stats_report_due */
bool tg_stats_report_due(struct tg_stats *stats, uint64_t now_cycles,
                         uint64_t cycles_per_second,
                         uint32_t report_interval_sec) {
        uint64_t interval;

        if (stats == NULL || cycles_per_second == 0 || report_interval_sec == 0)
                return false;
        interval = cycles_per_second * report_interval_sec;
        if (stats->last_report_cycles == 0) {
                stats->last_report_cycles = now_cycles;
                return false;
        }
        if (now_cycles < stats->last_report_cycles ||
            now_cycles - stats->last_report_cycles < interval)
                return false;
        stats->last_report_cycles = now_cycles;
        return true;
}
