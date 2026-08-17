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
        char header[4096];
        char record[4096];
        char *header_columns[128];
        char *record_columns[128];
        struct tg_stats_csv csv;
        struct tg_stats_snapshot snapshot = {0};
        int fd = mkstemp(path);

        assert(fd >= 0);
        assert(close(fd) == 0);
        assert(tg_stats_csv_open(&csv, path, 1000000) == 0);
        snapshot.worker_index = 0;
        snapshot.phase = TG_STATS_PHASE_PERIODIC;
        snapshot.udp_tx_queue_drops = 77;
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
        for (unsigned int i = 0; i < header_count; i++) {
                if (strcmp(header_columns[i], "udp_tx_queue_drops") == 0) {
                        assert(strcmp(record_columns[i], "77") == 0);
                        return 0;
                }
        }
        assert(!"udp_tx_queue_drops column is missing");
        return 1;
}
