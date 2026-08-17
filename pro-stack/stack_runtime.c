#include "stack_runtime.h"

#include "arp.h"
#include "config.h"
#include "icmp.h"
#include "log.h"
#include "ring.h"
#include "socket.h"

#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_timer.h>

#include <stdatomic.h>
#include <string.h>

static atomic_bool g_stop_requested;
static struct stack_runtime_worker *g_workers[RTE_MAX_LCORE];

int stack_runtime_worker_init(struct stack_runtime_worker *worker,
                              unsigned int lcore_id, uint16_t queue_id,
                              struct rte_mempool *mp,
                              struct inout_ring *ring,
                              stack_runtime_reactor_fn reactor,
                              void *reactor_ctx) {
        if (worker == NULL || mp == NULL || ring == NULL ||
            lcore_id >= RTE_MAX_LCORE)
                return -1;

        memset(worker, 0, sizeof(*worker));
        worker->lcore_id = lcore_id;
        worker->queue_id = queue_id;
        worker->mp = mp;
        worker->ring = ring;
        worker->reactor = reactor;
        worker->reactor_ctx = reactor_ctx;
        g_workers[lcore_id] = worker;
        atomic_store(&g_stop_requested, false);
        return 0;
}

int stack_runtime_queue_for_lcore(unsigned int lcore_id, uint16_t *queue_out) {
        if (queue_out == NULL || lcore_id >= RTE_MAX_LCORE ||
            g_workers[lcore_id] == NULL)
                return -1;
        *queue_out = g_workers[lcore_id]->queue_id;
        return 0;
}

void stack_runtime_request_stop(void) { atomic_store(&g_stop_requested, true); }

int stack_runtime_stop_requested(void) {
        return atomic_load(&g_stop_requested);
}

void stack_runtime_metrics_take(struct stack_runtime_metrics *out) {
        struct stack_runtime_worker *worker;
        struct nsock_tx_metrics tx = {0};

        if (out == NULL)
                return;

        if (rte_lcore_id() >= RTE_MAX_LCORE ||
            (worker = g_workers[rte_lcore_id()]) == NULL) {
                memset(out, 0, sizeof(*out));
                return;
        }
        *out = worker->metrics;
        nsock_tx_metrics_take(&tx);
        out->socket_scans += tx.dirty_dequeues;
        out->tx_flush_calls += tx.flush_calls;
        out->dirty_tx_enqueues = tx.dirty_enqueues;
        out->dirty_tx_dedup_hits = tx.dirty_dedup_hits;
        out->dirty_tx_dequeues = tx.dirty_dequeues;
        out->dirty_tx_requeues = tx.dirty_requeues;
        out->dirty_tx_arp_waits = tx.arp_waits;
        out->dirty_tx_arp_wakeups = tx.arp_wakeups;
        out->dirty_tx_budget_exhausted = tx.dirty_budget_exhausted;
        out->udp_tx_queue_drops = tx.udp_tx_queue_drops;
        out->dirty_tx_high_water = tx.dirty_high_water;
        out->dirty_tx_depth = tx.dirty_depth;
        memset(&worker->metrics, 0, sizeof(worker->metrics));
}

static int ipv4_rx_validate(const struct rte_mbuf *mbuf,
                            struct rte_ipv4_hdr **ip_out) {
        const uint16_t eth_len = sizeof(struct rte_ether_hdr);
        const uint16_t ip_len = sizeof(struct rte_ipv4_hdr);

        if (mbuf->pkt_len < eth_len + ip_len ||
            mbuf->data_len < eth_len + ip_len)
                return -1;

        struct rte_ipv4_hdr *ip =
            rte_pktmbuf_mtod_offset(mbuf, struct rte_ipv4_hdr *, eth_len);
        if ((ip->version_ihl >> 4) != 4 || rte_ipv4_hdr_len(ip) != sizeof(*ip))
                return -1;

        uint16_t total_len = rte_be_to_cpu_16(ip->total_length);
        if (total_len < ip_len || total_len > mbuf->pkt_len - eth_len ||
            rte_ipv4_cksum(ip) != 0)
                return -1;

        *ip_out = ip;
        return 0;
}

static void dispatch_packet(struct rte_mempool *mp, struct rte_mbuf *mbuf,
                            struct rte_ring *out) {
        if (mbuf->pkt_len < sizeof(struct rte_ether_hdr) ||
            mbuf->data_len < sizeof(struct rte_ether_hdr)) {
                rte_pktmbuf_free(mbuf);
                return;
        }

        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
#if ENABLE_ARP
        if (eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
                bool reply_allowed =
                    (mbuf->dynfield1[0] & ARP_MBUF_F_LEARN_ONLY) == 0;

                arp_handle_mode(mp, mbuf, out, reply_allowed);
                return;
        }
#endif
        if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                rte_pktmbuf_free(mbuf);
                return;
        }

        struct rte_ipv4_hdr *ip;
        if (ipv4_rx_validate(mbuf, &ip) != 0) {
                rte_pktmbuf_free(mbuf);
                return;
        }

        switch (ip->next_proto_id) {
#if ENABLE_ICMP
        case IPPROTO_ICMP:
                icmp_handle(mp, mbuf, out);
                return;
#endif
        default: {
                const struct sock_ops *ops = sock_ops_lookup(ip->next_proto_id);
                if (ops != NULL && ops->ingress != NULL) {
                        ops->ingress(mbuf);
                        return;
                }
                rte_pktmbuf_free(mbuf);
                return;
        }
        }
}

int stack_runtime_worker_entry(void *arg) {
        struct stack_runtime_worker *worker = arg;
        struct rte_mempool *mp;
        struct inout_ring *ring;
        struct stack_runtime_metrics *metrics;
        const uint64_t timer_interval =
            rte_get_timer_hz() * TIMER_MANAGE_INTERVAL_MS / 1000;
        const uint64_t arp_maintenance_interval =
            rte_get_timer_hz() * ARP_MAINTENANCE_INTERVAL_MS / 1000;
#if ENABLE_ARP_SWEEP
        const uint64_t arp_sweep_interval =
            rte_get_timer_hz() * ARP_SWEEP_INTERVAL_MS / 1000;
#endif

        if (worker == NULL || worker->lcore_id != rte_lcore_id() ||
            worker->ring == NULL || worker->mp == NULL)
                return -1;
        mp = worker->mp;
        ring = worker->ring;
        metrics = &worker->metrics;
        LOG_INFO("packet worker started lcore=%u queue=%u", rte_lcore_id(),
                 worker->queue_id);
        while (!stack_runtime_stop_requested()) {
                struct rte_mbuf *mbufs[BURST_SIZE];
                uint64_t turn_start = rte_get_timer_cycles();
                uint64_t phase_start;

                socket_owner_process_commands();

                /* Sample occupancy before this turn drains the RX ring. */
                phase_start = rte_get_timer_cycles();
                unsigned int in_depth = rte_ring_count(ring->in);
                if (in_depth > metrics->in_ring_high_water)
                        metrics->in_ring_high_water = in_depth;
                unsigned int nb_rx = rte_ring_sc_dequeue_burst(
                    ring->in, (void **)mbufs, BURST_SIZE, NULL);
                for (unsigned int i = 0; i < nb_rx; i++)
                        dispatch_packet(mp, mbufs[i], ring->out);
                metrics->rx_packets += nb_rx;
                metrics->rx_cycles += rte_get_timer_cycles() - phase_start;

                socket_owner_process_commands();

                /* Timers are sampled separately from packet and app work. */
                phase_start = rte_get_timer_cycles();
                uint64_t now = rte_get_timer_cycles();
                if (now - worker->last_timer_tsc >= timer_interval) {
                        rte_timer_manage();
                        worker->last_timer_tsc = now;
                }
                if (now - worker->last_arp_maintenance_tsc >=
                    arp_maintenance_interval) {
                        arp_maintain(now);
                        worker->last_arp_maintenance_tsc = now;
                }
#if ENABLE_ARP_SWEEP
                if (now - worker->last_arp_sweep_tsc >= arp_sweep_interval) {
                        arp_debug_sweep(mp, ring->out, now);
                        worker->last_arp_sweep_tsc = now;
                }
#endif
                metrics->maintenance_cycles +=
                    rte_get_timer_cycles() - phase_start;

                /* The reactor runs after ingress so completions free slots. */
                phase_start = rte_get_timer_cycles();
                if (worker->reactor != NULL)
                        worker->reactor(worker->reactor_ctx, BURST_SIZE);
                metrics->reactor_cycles +=
                    rte_get_timer_cycles() - phase_start;

                /* Flush only sockets that have runnable TX work. */
                phase_start = rte_get_timer_cycles();
                (void)nsock_tx_dirty_drain(mp, TX_DIRTY_BUDGET);
                metrics->tx_flush_cycles +=
                    rte_get_timer_cycles() - phase_start;
                unsigned int out_depth = rte_ring_count(ring->out);
                if (out_depth > metrics->out_ring_high_water)
                        metrics->out_ring_high_water = out_depth;
                metrics->worker_turns++;
                metrics->turn_cycles += rte_get_timer_cycles() - turn_start;
        }
        return 0;
}
