/**
 * @file stats_csv.c
 * @brief Implements the buffered traffic-generator CSV sink.
 */

#include "stats_csv.h"

#include <inttypes.h>
#include <string.h>

static uint64_t tg_csv_cycles_to_us(uint64_t cycles, uint64_t hz) {
        uint64_t whole_seconds;
        uint64_t remainder;

        if (hz == 0)
                return 0;
        whole_seconds = cycles / hz;
        remainder = cycles % hz;
        return whole_seconds * UINT64_C(1000000) +
               remainder * UINT64_C(1000000) / hz;
}

static uint64_t tg_csv_average_us(uint64_t cycles, uint64_t samples,
                                  uint64_t hz) {
        return samples == 0 ? 0 : tg_csv_cycles_to_us(cycles / samples, hz);
}

static const char *tg_csv_phase(uint64_t phase) {
        return phase == TG_STATS_PHASE_FINAL ? "final" : "periodic";
}

int tg_stats_csv_open(struct tg_stats_csv *csv, const char *path,
                      uint64_t cycles_per_second) {
        if (csv == NULL || path == NULL || path[0] == '\0' ||
            cycles_per_second == 0)
                return -1;
        memset(csv, 0, sizeof(*csv));
        csv->file = fopen(path, "w");
        if (csv->file == NULL)
                return -1;
        csv->path = path;
        csv->cycles_per_second = cycles_per_second;
        (void)setvbuf(csv->file, NULL, _IOFBF, 64 * 1024);
        if (fprintf(
                csv->file,
                "scope,phase,timestamp_us,sequence,worker,lcore,"
                "started,done,success,fail,fail_connect,fail_io,fail_proto,"
                "fail_resource,deferred_resource,bytes_tx,bytes_rx,"
                "http_success_total,active,live_sockets,"
                "connect_samples,connect_sum_cycles,connect_avg_us,"
                "connect_max_us,first_rx_samples,first_rx_sum_cycles,"
                "first_rx_avg_us,first_rx_max_us,complete_samples,"
                "complete_sum_cycles,complete_avg_us,complete_max_us,"
                "memory_paused,memory_pauses,tx_available,tx_peak,"
                "payload_available,payload_peak,tx_alloc_fail,worker_turns,"
                "rx_packets,socket_scans,tx_flush_calls,dirty_tx_enqueues,"
                "dirty_tx_dedup_hits,dirty_tx_requeues,dirty_tx_arp_waits,"
                "dirty_tx_arp_wakeups,dirty_tx_depth,dirty_tx_high_water,"
                "dirty_tx_budget_exhausted,turn_avg_us,rx_us,maintenance_us,"
                "reactor_us,tx_flush_us,reactor_turns,reactor_events,"
                "reactor_burst_high_water,scheduler_starts,tokens,"
                "socket_releases,ring_hwm_in,ring_hwm_out,rx_ring_drops,"
                "tx_nic_drops,udp_tx_queue_drops,rx_owner_hits,"
                "rx_software_hashes,"
                "rx_parse_fallbacks,stats_queue_drops,connections_created,"
                "connections_reused,ofo_segments_current,ofo_segments_peak,"
                "ofo_bytes_current,ofo_bytes_peak,ofo_accepted_segments,"
                "ofo_accepted_bytes,ofo_released_segments,ofo_released_bytes,"
                "ofo_reorder_distance_max,ofo_drop_rcvbuf,"
                "ofo_drop_seg_limit,ofo_drop_byte_limit,ofo_drop_owner_limit,"
                "ofo_drop_alloc,ofo_drop_pressure,ofo_pressure_transitions,"
                "ofo_pressure_active\n") < 0) {
                (void)fclose(csv->file);
                memset(csv, 0, sizeof(*csv));
                return -1;
        }
        return 0;
}

int tg_stats_csv_write(struct tg_stats_csv *csv,
                       const struct tg_stats_snapshot *snapshot) {
        const char *scope;
        uint64_t hz;
        int result;

        if (csv == NULL || csv->file == NULL || snapshot == NULL || csv->failed)
                return -1;
        scope = snapshot->worker_index == UINT64_MAX ? "aggregate" : "worker";
        hz = csv->cycles_per_second;
        result = fprintf(
            csv->file,
            "%s,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
            scope, tg_csv_phase(snapshot->phase),
            tg_csv_cycles_to_us(snapshot->timestamp_cycles, hz),
            snapshot->sequence, snapshot->worker_index, snapshot->lcore_id,
            snapshot->txns_started, snapshot->txns_done, snapshot->txns_success,
            snapshot->txns_fail, snapshot->fail_connect, snapshot->fail_io,
            snapshot->fail_proto, snapshot->fail_resource,
            snapshot->starts_deferred_resource, snapshot->bytes_tx,
            snapshot->bytes_rx, snapshot->http_rps_total, snapshot->concurrency,
            snapshot->live_sockets, snapshot->connect_samples,
            snapshot->connect_cycles,
            tg_csv_average_us(snapshot->connect_cycles,
                              snapshot->connect_samples, hz),
            tg_csv_cycles_to_us(snapshot->connect_max_cycles, hz),
            snapshot->first_rx_samples, snapshot->first_rx_cycles,
            tg_csv_average_us(snapshot->first_rx_cycles,
                              snapshot->first_rx_samples, hz),
            tg_csv_cycles_to_us(snapshot->first_rx_max_cycles, hz),
            snapshot->complete_samples, snapshot->complete_cycles,
            tg_csv_average_us(snapshot->complete_cycles,
                              snapshot->complete_samples, hz),
            tg_csv_cycles_to_us(snapshot->complete_max_cycles, hz),
            snapshot->memory_paused, snapshot->memory_pauses,
            snapshot->tx_available, snapshot->tx_peak,
            snapshot->payload_available, snapshot->payload_peak,
            snapshot->tx_alloc_fail, snapshot->worker_turns,
            snapshot->rx_packets, snapshot->socket_scans,
            snapshot->tx_flush_calls, snapshot->dirty_tx_enqueues,
            snapshot->dirty_tx_dedup_hits, snapshot->dirty_tx_requeues,
            snapshot->dirty_tx_arp_waits, snapshot->dirty_tx_arp_wakeups,
            snapshot->dirty_tx_depth, snapshot->dirty_tx_high_water,
            snapshot->dirty_tx_budget_exhausted,
            tg_csv_average_us(snapshot->turn_cycles, snapshot->worker_turns,
                              hz),
            tg_csv_cycles_to_us(snapshot->rx_cycles, hz),
            tg_csv_cycles_to_us(snapshot->maintenance_cycles, hz),
            tg_csv_cycles_to_us(snapshot->reactor_cycles, hz),
            tg_csv_cycles_to_us(snapshot->tx_flush_cycles, hz),
            snapshot->reactor_turns, snapshot->reactor_events,
            snapshot->reactor_burst_high_water, snapshot->scheduler_starts,
            snapshot->tokens, snapshot->socket_releases, snapshot->ring_hwm_in,
            snapshot->ring_hwm_out, snapshot->rx_ring_drops,
            snapshot->tx_nic_drops, snapshot->udp_tx_queue_drops,
            snapshot->rx_owner_hits, snapshot->rx_software_hashes,
            snapshot->rx_parse_fallbacks, snapshot->stats_queue_drops,
            snapshot->connections_created, snapshot->connections_reused,
            snapshot->ofo_segments_current, snapshot->ofo_segments_peak,
            snapshot->ofo_bytes_current, snapshot->ofo_bytes_peak,
            snapshot->ofo_accepted_segments, snapshot->ofo_accepted_bytes,
            snapshot->ofo_released_segments, snapshot->ofo_released_bytes,
            snapshot->ofo_reorder_distance_max, snapshot->ofo_drop_rcvbuf,
            snapshot->ofo_drop_seg_limit, snapshot->ofo_drop_byte_limit,
            snapshot->ofo_drop_owner_limit, snapshot->ofo_drop_alloc,
            snapshot->ofo_drop_pressure,
            snapshot->ofo_pressure_transitions,
            snapshot->ofo_pressure_active);
        if (result < 0)
                csv->failed = true;
        return result < 0 ? -1 : 0;
}

int tg_stats_csv_close(struct tg_stats_csv *csv) {
        int result = 0;

        if (csv == NULL)
                return -1;
        if (csv->file != NULL) {
                if (fflush(csv->file) != 0)
                        result = -1;
                if (fclose(csv->file) != 0)
                        result = -1;
        }
        csv->file = NULL;
        return result;
}
