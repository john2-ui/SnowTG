#ifndef TRAFFIC_GEN_STATS_CSV_H
#define TRAFFIC_GEN_STATS_CSV_H

/**
 * @file stats_csv.h
 * @brief Buffered single-writer CSV sink for traffic-generator snapshots.
 */

#include "stats.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct tg_stats_csv {
        FILE *file;
        const char *path;
        uint64_t cycles_per_second;
        bool failed;
};

/** Open a path and write the stable CSV header. */
int tg_stats_csv_open(struct tg_stats_csv *csv, const char *path,
                      uint64_t cycles_per_second);

/** Write one worker or aggregate snapshot. */
int tg_stats_csv_write(struct tg_stats_csv *csv,
                       const struct tg_stats_snapshot *snapshot);

/** Flush and close the sink. */
int tg_stats_csv_close(struct tg_stats_csv *csv);

#endif /* TRAFFIC_GEN_STATS_CSV_H */
