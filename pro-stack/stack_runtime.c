#include "stack_runtime.h"

#include "arp.h"
#include "config.h"
#include "icmp.h"
#include "log.h"
#include "owner_timer.h"
#include "ring.h"
#include "rx_dispatch.h"
#include "ipv4_reassembly.h"
#include "socket.h"
#include "tcp.h"

#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

#include <stdatomic.h>
#include <limits.h>
#include <string.h>

static atomic_bool g_stop_requested;
static struct stack_runtime_worker *g_workers[RTE_MAX_LCORE];
static struct stack_runtime_worker *g_queue_workers[RTE_MAX_LCORE];

void stack_runtime_tx_drain(struct stack_runtime_worker *worker,
                            unsigned int burst_budget, bool sample) {
        if (worker == NULL || !worker->direct_tx_enabled ||
            worker->lcore_id != rte_lcore_id())
                return;
        for (unsigned int burst = 0; burst < burst_budget; burst++) {
                struct rte_mbuf *packets[BURST_SIZE];
                unsigned int count = rte_ring_dequeue_burst_start(
                    worker->ring->out, (void **)packets, BURST_SIZE, NULL);
                if (count == 0)
                        break;
                uint64_t start = sample ? rte_get_timer_cycles() : 0;
                unsigned int sent = rte_eth_tx_burst(worker->port_id,
                    worker->tx_queue_id, packets, count);
                if (sample) {
                        worker->metrics.nic_tx_cycles += rte_get_timer_cycles() - start;
                        worker->metrics.nic_tx_sampled_packets += sent;
                        worker->metrics.nic_tx_sampled_bursts++;
                }
                /* Keep unsent packets at the head of this SPSC ring. NIC
                 * descriptor pressure is backpressure, not packet loss.
                 * At shutdown producers have stopped; reclaim any leftovers.
                 */
                if (burst_budget == UINT_MAX) {
                        for (unsigned int i = sent; i < count; i++)
                                rte_pktmbuf_free(packets[i]);
                        worker->metrics.tx_nic_drops += count - sent;
                }
                rte_ring_dequeue_finish(worker->ring->out,
                    burst_budget == UINT_MAX ? count : sent);
                worker->metrics.tx_packets += sent;
                worker->metrics.tx_bursts++;
                if (sent != count && burst_budget != UINT_MAX)
                        break;
        }
}

int stack_runtime_worker_init(struct stack_runtime_worker *worker,
                              unsigned int lcore_id, uint16_t queue_id,
                              uint32_t timer_capacity, struct rte_mempool *mp,
                              struct inout_ring *ring,
                              stack_runtime_reactor_fn reactor,
                              void *reactor_ctx) {
        if (worker == NULL || mp == NULL || ring == NULL ||
            lcore_id >= RTE_MAX_LCORE || queue_id >= RTE_MAX_LCORE ||
            timer_capacity == 0)
                return -1;

        memset(worker, 0, sizeof(*worker));
        worker->lcore_id = lcore_id;
        worker->queue_id = queue_id;
        worker->mp = mp;
        worker->ring = ring;
        worker->reactor = reactor;
        worker->reactor_ctx = reactor_ctx;
        if (owner_timer_engine_init(&worker->timer_engine, lcore_id,
                                    timer_capacity) != 0)
                return -1;
        g_workers[lcore_id] = worker;
        g_queue_workers[queue_id] = worker;
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
        struct tcp_ofo_metrics ofo = {0};

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
        tcp_ofo_metrics_take(&ofo);
        out->ofo_segments_current = ofo.segments_current;
        out->ofo_segments_peak = ofo.segments_peak;
        out->ofo_bytes_current = ofo.bytes_current;
        out->ofo_bytes_peak = ofo.bytes_peak;
        out->ofo_accepted_segments = ofo.accepted_segments;
        out->ofo_accepted_bytes = ofo.accepted_bytes;
        out->ofo_released_segments = ofo.released_segments;
        out->ofo_released_bytes = ofo.released_bytes;
        out->ofo_reorder_distance_max = ofo.reorder_distance_max;
        out->ofo_drop_rcvbuf = ofo.drop_rcvbuf;
        out->ofo_drop_seg_limit = ofo.drop_seg_limit;
        out->ofo_drop_byte_limit = ofo.drop_byte_limit;
        out->ofo_drop_owner_limit = ofo.drop_owner_limit;
        out->ofo_drop_alloc = ofo.drop_alloc;
        out->ofo_drop_pressure = ofo.drop_pressure;
        out->ofo_pressure_transitions = ofo.pressure_transitions;
        out->ofo_pressure_active = ofo.pressure_active;
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

static void rx_forward(struct stack_runtime_worker *from, uint16_t queue,
                        struct rte_mbuf *mbuf) {
        struct stack_runtime_worker *to =
            queue < RTE_MAX_LCORE ? g_queue_workers[queue] : NULL;
        if (to == NULL || !to->direct_rx_enabled ||
            rte_ring_mp_enqueue(to->ring->in, mbuf) != 0) {
                from->metrics.rx_handoff_drops++;
                rte_pktmbuf_free(mbuf);
        } else {
                from->metrics.rx_handoffs++;
        }
}

void stack_runtime_rx_process(struct stack_runtime_worker *worker,
                              struct rte_mbuf *mbuf, uint64_t now_cycles) {
        struct rx_dispatch_result result;
        if (mbuf->data_len >= sizeof(struct rte_ether_hdr)) {
                const struct rte_ether_hdr *eth =
                    rte_pktmbuf_mtod(mbuf, const struct rte_ether_hdr *);
                if (eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
                        if (!(mbuf->dynfield1[0] & ARP_MBUF_F_LEARN_ONLY)) {
                                if (worker->queue_id != 0) {
                                        rx_forward(worker, 0, mbuf);
                                        return;
                                }
                                for (unsigned int q = 1; q < RTE_MAX_LCORE; q++) {
                                        if (g_queue_workers[q] == NULL ||
                                            !g_queue_workers[q]->direct_rx_enabled)
                                                continue;
                                        struct rte_mbuf *clone =
                                            rte_pktmbuf_clone(mbuf, worker->mp);
                                        if (clone == NULL) {
                                                worker->metrics.rx_handoff_drops++;
                                                continue;
                                        }
                                        clone->dynfield1[0] = ARP_MBUF_F_LEARN_ONLY;
                                        rx_forward(worker, q, clone);
                                }
                        }
                        goto deliver;
                }
                if (worker->queue_id != 0 &&
                    eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) &&
                    mbuf->data_len >= sizeof(*eth) + sizeof(struct rte_ipv4_hdr)) {
                        const struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(
                            mbuf, const struct rte_ipv4_hdr *, sizeof(*eth));
                        if (rte_be_to_cpu_16(ip->fragment_offset) &
                            (RTE_IPV4_HDR_OFFSET_MASK | RTE_IPV4_HDR_MF_FLAG | 0x8000)) {
                                rx_forward(worker, 0, mbuf);
                                return;
                        }
                }
        }
        if (worker->reassembly != NULL) {
                mbuf = ipv4_reassembly_process(worker->reassembly, mbuf, now_cycles);
                if (mbuf == NULL)
                        return;
        }
        rx_dispatch_classify(mbuf, worker->rx_queue_id, &result);
        if (result.worker_index != worker->queue_id) {
                rx_forward(worker, result.worker_index, mbuf);
                return;
        }
deliver:
        dispatch_packet(worker->mp, mbuf, worker->ring->out);
        worker->metrics.rx_packets++;
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
                for (unsigned int i = 0; i < nb_rx; i++) {
                        if (worker->direct_rx_enabled)
                                stack_runtime_rx_process(worker, mbufs[i], phase_start);
                        else
                                dispatch_packet(mp, mbufs[i], ring->out);
                }
                if (worker->direct_rx_enabled) {
                        nb_rx = rte_eth_rx_burst(worker->port_id,
                                                worker->rx_queue_id, mbufs, BURST_SIZE);
                        metrics->nic_rx_packets += nb_rx;
                        metrics->rx_burst_calls++;
                        metrics->rx_empty_bursts += nb_rx == 0;
                        metrics->rx_full_bursts += nb_rx == BURST_SIZE;
                        for (unsigned int i = 0; i < nb_rx; i++) {
                                /* NIC mbufs may retain metadata from an old clone. */
                                mbufs[i]->dynfield1[0] = 0;
                                stack_runtime_rx_process(worker, mbufs[i], phase_start);
                        }
                } else {
                        metrics->rx_packets += nb_rx;
                }
                metrics->rx_cycles += rte_get_timer_cycles() - phase_start;

                socket_owner_process_commands();

                /* Timers are sampled separately from packet and app work. */
                phase_start = rte_get_timer_cycles();
                uint64_t now = phase_start;
                if (worker->direct_rx_enabled && worker->reassembly != NULL)
                        ipv4_reassembly_maintain(worker->reassembly, now);
                if (now - worker->last_timer_tsc >= timer_interval) {
                        (void)owner_timer_poll(&worker->timer_engine);
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
                if (worker->direct_tx_enabled) {
                        bool sample = false;
                        if (worker->tx_sample_every != 0) {
                                if (worker->tx_until_sample == 0) {
                                        sample = true;
                                        worker->tx_until_sample = worker->tx_sample_every;
                                }
                                worker->tx_until_sample--;
                        }
                        stack_runtime_tx_drain(worker, 4, sample);
                }
                metrics->worker_turns++;
                metrics->turn_cycles += rte_get_timer_cycles() - turn_start;
        }
        stack_runtime_tx_drain(worker, UINT_MAX, false);
        if (worker->on_exit != NULL)
                worker->on_exit(worker->reactor_ctx);
        owner_timer_engine_fini(&worker->timer_engine);
        return 0;
}
