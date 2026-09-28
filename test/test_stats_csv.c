#include "../traffic-gen/core/stats_csv.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned int split_csv(char *line, char **columns,
                              unsigned int capacity) {
        unsigned int count = 0;
        char *save = NULL;

        for (char *column = strtok_r(line, ",\n", &save);
             column != NULL && count < capacity;
             column = strtok_r(NULL, ",\n", &save))
                columns[count++] = column;
        return count;
}

int main(void) {
        char path[] = "/tmp/tg-stats-csv-XXXXXX";
        char header[16384];
        char record[16384];
        char *header_columns[512];
        char *record_columns[512];
        struct tg_stats_csv csv;
        struct tg_stats_snapshot snapshot = {0};
        int fd = mkstemp(path);

        assert(fd >= 0);
        assert(close(fd) == 0);
        assert(tg_stats_csv_open(&csv, path, 1000000) == 0);
        snapshot.resources.version = snapshot.resources.complete = 1;
        snapshot.error_reasons[TG_ERROR_REDIS_ERROR] = 19;
        snapshot.resources.values[TG_RESOURCE_tcp_payload] = (struct resource_metric){
            .capacity = 10, .current = 2, .peak = 7, .exhausted = UINT64_C(4294967296)};
        struct tg_stats_snapshot total = {0}, aggregate = {0}, copy = {0};
        tg_stats_snapshot_add_runtime(&total, &snapshot);
        tg_stats_snapshot_add_runtime(&total, &snapshot);
        tg_stats_snapshot_copy_runtime(&copy, &total);
        assert(memcmp(&copy.resources, &snapshot.resources, sizeof(copy.resources)) == 0);
        tg_stats_snapshot_add(&aggregate, &snapshot);
        tg_stats_snapshot_add(&aggregate, &snapshot);
        assert(aggregate.resources.complete == 2);
        assert(aggregate.error_reasons[TG_ERROR_REDIS_ERROR] == 38);
        assert(aggregate.resources.values[TG_RESOURCE_tcp_payload].current == 4);
        assert(aggregate.resources.values[TG_RESOURCE_tcp_payload].capacity == 20);
        assert(aggregate.resources.values[TG_RESOURCE_tcp_payload].peak == 7);
        assert(aggregate.resources.values[TG_RESOURCE_tcp_payload].exhausted == UINT64_C(8589934592));
        snapshot.worker_index = 0;
        snapshot.error_reasons[TG_ERROR_RESET] = 17;
        snapshot.phase = TG_STATS_PHASE_PERIODIC;
        snapshot.udp_tx_queue_drops = 77;
        snapshot.ofo_drop_pressure = 88;
        snapshot.ofo_pressure_active = 1;
        snapshot.tcp_drain_residual = 9;
        snapshot.tcp_forced_cleanup = 7;
        snapshot.tcp_pool_objects_in_use = 5;
        assert(tg_stats_csv_write(&csv, &snapshot) == 0);
        assert(tg_stats_csv_close(&csv) == 0);

        FILE *file = fopen(path, "r");
        assert(file != NULL);
        assert(fgets(header, sizeof(header), file) != NULL);
        assert(fgets(record, sizeof(record), file) != NULL);
        assert(fclose(file) == 0);
        assert(unlink(path) == 0);

        unsigned int header_count =
            split_csv(header, header_columns,
                      sizeof(header_columns) / sizeof(header_columns[0]));
        unsigned int record_count =
            split_csv(record, record_columns,
                      sizeof(record_columns) / sizeof(record_columns[0]));
        assert(header_count == record_count);
        bool found_resource = false;
        bool found_error = false;
        bool found_redis = false;
        bool found_udp = false;
        bool found_ofo_drop = false;
        bool found_ofo_pressure = false;
        bool found_drain_residual = false;
        bool found_forced_cleanup = false;
        bool found_pool_in_use = false;
        for (unsigned int i = 0; i < header_count; i++) {
                if (strcmp(header_columns[i], "error_redis_error") == 0) {
                        assert(strcmp(record_columns[i], "19") == 0);
                        found_redis = true;
                }
                if (strcmp(header_columns[i], "res_tcp_payload_exhausted") == 0) {
                        assert(strcmp(record_columns[i], "4294967296") == 0);
                        found_resource = true;
                }
                if (strcmp(header_columns[i], "error_reset") == 0) {
                        assert(strcmp(record_columns[i], "17") == 0);
                        found_error = true;
                }
                if (strcmp(header_columns[i], "udp_tx_queue_drops") == 0) {
                        assert(strcmp(record_columns[i], "77") == 0);
                        found_udp = true;
                }
                if (strcmp(header_columns[i], "ofo_drop_pressure") == 0) {
                        assert(strcmp(record_columns[i], "88") == 0);
                        found_ofo_drop = true;
                }
                if (strcmp(header_columns[i], "ofo_pressure_active") == 0) {
                        assert(strcmp(record_columns[i], "1") == 0);
                        found_ofo_pressure = true;
                }
                if (strcmp(header_columns[i], "tcp_drain_residual") == 0) {
                        assert(strcmp(record_columns[i], "9") == 0);
                        found_drain_residual = true;
                }
                if (strcmp(header_columns[i], "tcp_forced_cleanup") == 0) {
                        assert(strcmp(record_columns[i], "7") == 0);
                        found_forced_cleanup = true;
                }
                if (strcmp(header_columns[i],
                           "tcp_pool_objects_in_use") == 0) {
                        assert(strcmp(record_columns[i], "5") == 0);
                        found_pool_in_use = true;
                }
        }
        assert(found_resource);
        assert(found_error);
        assert(found_redis);
        assert(found_udp);
        assert(found_ofo_drop);
        assert(found_ofo_pressure);
        assert(found_drain_residual);
        assert(found_forced_cleanup);
        assert(found_pool_in_use);
        return 0;
}
