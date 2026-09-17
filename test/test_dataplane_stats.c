/**
 * @file test_dataplane_stats.c
 * @brief Tests sampling cadence and NIC CSV deltas without a device.
 *
 * A local statistics stub controls read errors and counter resets so the
 * report's validity flags and recovery deltas can be checked deterministically.
 */

#include "../traffic-gen/core/dataplane_stats.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct rte_eth_stats fake_nic;
static int fake_error;

/** Supplies the scripted NIC snapshot and status in place of a device read. */
int rte_eth_stats_get(uint16_t port, struct rte_eth_stats *out) {
        assert(port == 0);
        *out = fake_nic;
        return fake_error;
}

/** @brief Verifies the CSV rows and disabled/every-loop sampling boundaries. */
int main(void) {
        struct tg_dataplane_stats stats;
        char path[] = "/tmp/tg-dataplane-XXXXXX";
        char line[4096];
        int fd = mkstemp(path);
        assert(fd >= 0);
        close(fd);
        fake_nic.ipackets = 100;
        assert(tg_dataplane_stats_open(&stats, path, 0, 64) == 0);
        /* Sampling starts immediately, selecting loops 0, 64, and 128. */
        for (unsigned int i = 0; i < 130; i++)
                assert(tg_dataplane_sample(&stats) == (i % 64 == 0));
        assert(stats.main.main_loops == 130 && stats.main.sampled_loops == 3);
        /* Force each row with final=true to avoid waiting for report deadlines. */
        fake_nic.ipackets = 150;
        tg_dataplane_stats_report(&stats, stats.start_cycles + 100, true);
        /* A failed read must preserve 150 as the next interval's baseline. */
        fake_error = -EIO;
        tg_dataplane_stats_report(&stats, stats.start_cycles + 200, true);
        fake_error = 0;
        fake_nic.ipackets = 175;
        tg_dataplane_stats_report(&stats, stats.start_cycles + 300, true);
        /* A counter reset invalidates one row, then rebases the next delta. */
        fake_nic.ipackets = 2;
        tg_dataplane_stats_report(&stats, stats.start_cycles + 400, true);
        fake_nic.ipackets = 5;
        tg_dataplane_stats_report(&stats, stats.start_cycles + 500, true);
        assert(tg_dataplane_stats_close(&stats) == 0);
        FILE *file = fopen(path, "r");
        assert(file != NULL);
        assert(fgets(line, sizeof(line), file) != NULL);
        /* Resolve fields by name so unrelated CSV columns can be extended. */
        unsigned int valid_column = 0, packets_column = 0, columns = 0;
        for (char *p = strtok(line, ",\n"); p; p = strtok(NULL, ",\n"), columns++) {
                if (strcmp(p, "nic_delta_valid") == 0) valid_column = columns;
                if (strcmp(p, "nic_ipackets") == 0) packets_column = columns;
        }
        const unsigned int expected_valid[] = {1, 0, 1, 0, 1};
        const unsigned int expected_packets[] = {50, 0, 25, 0, 3};
        for (unsigned int row = 0; row < 5; row++) {
                assert(fgets(line, sizeof(line), file) != NULL);
                unsigned int col = 0;
                for (char *p = strtok(line, ",\n"); p; p = strtok(NULL, ",\n"), col++) {
                        if (col == valid_column) assert(strtoul(p, NULL, 10) == expected_valid[row]);
                        if (col == packets_column) assert(strtoul(p, NULL, 10) == expected_packets[row]);
                }
                assert(col == columns);
        }
        fclose(file);
        unlink(path);
        /* The sampler only checks for non-NULL; this sentinel is never used
         * for I/O. Verify that zero disables timing and one samples every loop. */
        stats.file = (FILE *)&stats;
        stats.sample_every = 0;
        assert(!tg_dataplane_sample(&stats));
        stats.sample_every = 1;
        stats.until_sample = 0;
        assert(tg_dataplane_sample(&stats));
        assert(tg_dataplane_sample(&stats));
        return 0;
}
