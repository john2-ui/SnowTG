/**
 * @file dataplane_stats.c
 * @brief Implements buffered main-lcore and NIC interval CSV reporting.
 *
 * Main counters remain cumulative across rows. NIC deltas use the last
 * successful read so transient read failures do not discard an interval.
 */

#include "dataplane_stats.h"

#include <rte_cycles.h>
#include <rte_lcore.h>
#include <inttypes.h>
#include <string.h>

/** NIC counters sharing CSV column order and reset/delta handling. */
#define TG_NIC_FIELDS(X)                                                      \
        X(ipackets) X(opackets) X(ibytes) X(obytes) X(imissed) X(ierrors)        \
        X(oerrors) X(rx_nombuf)

/** @copydoc tg_dataplane_stats_open */
int tg_dataplane_stats_open(struct tg_dataplane_stats *stats, const char *path,
                            uint16_t port_id, uint32_t sample_every) {
        memset(stats, 0, sizeof(*stats));
        if (path == NULL)
                return 0;
        stats->file = fopen(path, "w");
        if (stats->file == NULL)
                return -1;
        /* Buffer reports to reduce file I/O in the main polling loop. */
        (void)setvbuf(stats->file, NULL, _IOFBF, 64 * 1024);
        stats->port_id = port_id;
        stats->sample_every = sample_every;
        stats->start_cycles = rte_get_timer_cycles();
        stats->next_report_cycles = stats->start_cycles + rte_get_timer_hz();
        stats->nic_snapshot_cycles = stats->start_cycles;
        stats->nic_baseline_valid =
            rte_eth_stats_get(port_id, &stats->previous_nic) == 0;
        fprintf(stats->file, "phase,elapsed_cycles,timer_hz,lcore,port,"
                             "sample_every");
#define TG_COLUMN(name) fprintf(stats->file, "," #name);
        TG_MAIN_METRIC_FIELDS(TG_COLUMN)
#undef TG_COLUMN
        fprintf(stats->file, ",nic_rc,nic_delta_valid,nic_interval_cycles");
#define TG_COLUMN(name) fprintf(stats->file, ",nic_" #name);
        TG_NIC_FIELDS(TG_COLUMN)
#undef TG_COLUMN
        fputs(",link_rc,link_up,link_mbps,link_duplex\n", stats->file);
        if (ferror(stats->file)) {
                (void)fclose(stats->file);
                stats->file = NULL;
                return -1;
        }
        return 0;
}

/** @copydoc tg_dataplane_stats_report */
void tg_dataplane_stats_report(struct tg_dataplane_stats *stats, uint64_t now,
                               bool final) {
        struct rte_eth_stats nic = {0};
        struct rte_eth_link link = {0};
        bool valid;
        int rc;

        if (stats->file == NULL || stats->failed ||
            (!final && now < stats->next_report_cycles))
                return;
        rc = rte_eth_stats_get(stats->port_id, &nic);
        valid = rc == 0 && stats->nic_baseline_valid;
        /* A decrease in any field invalidates the entire NIC interval. */
#define TG_CHECK(name)                                                        \
        if (nic.name < stats->previous_nic.name) valid = false;
        TG_NIC_FIELDS(TG_CHECK)
#undef TG_CHECK
        fprintf(stats->file, "%s,%" PRIu64 ",%" PRIu64 ",%u,%u,%u",
                final ? "final" : "periodic", now - stats->start_cycles,
                rte_get_timer_hz(), rte_lcore_id(), stats->port_id,
                stats->sample_every);
#define TG_VALUE(name) fprintf(stats->file, ",%" PRIu64, stats->main.name);
        TG_MAIN_METRIC_FIELDS(TG_VALUE)
#undef TG_VALUE
        fprintf(stats->file, ",%d,%u,%" PRIu64, rc, valid,
                now - stats->nic_snapshot_cycles);
#define TG_DELTA(name)                                                        \
        fprintf(stats->file, ",%" PRIu64,                                     \
                valid ? nic.name - stats->previous_nic.name : UINT64_C(0));
        TG_NIC_FIELDS(TG_DELTA)
#undef TG_DELTA
        /* Query without blocking Main. A failed query leaves zero fields;
         * consumers must use link_rc to distinguish unknown from link-down. */
        int link_rc = rte_eth_link_get_nowait(stats->port_id, &link);
        fprintf(stats->file, ",%d,%u,%u,%u\n", link_rc, link.link_status,
                link.link_speed, link.link_duplex);
        /* Keep the baseline on read failure; rebase after a counter reset. */
        if (rc == 0) {
                stats->previous_nic = nic;
                stats->nic_baseline_valid = true;
                stats->nic_snapshot_cycles = now;
        }
        stats->next_report_cycles = now + rte_get_timer_hz();
        stats->failed = ferror(stats->file) != 0;
}

/** @copydoc tg_dataplane_stats_close */
int tg_dataplane_stats_close(struct tg_dataplane_stats *stats) {
        int rc = stats->failed ? -1 : 0;
        if (stats->file != NULL && fclose(stats->file) != 0)
                rc = -1;
        stats->file = NULL;
        return rc;
}
