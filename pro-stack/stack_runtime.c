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

static stack_runtime_reactor_fn g_reactor;
static void *g_reactor_ctx;

void stack_runtime_set_reactor(stack_runtime_reactor_fn fn, void *ctx) {
        g_reactor = fn;
        g_reactor_ctx = ctx;
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
                arp_handle(mp, mbuf, out);
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
        struct rte_mempool *mp = arg;
        struct inout_ring *ring = ring_instance();
        uint64_t last_timer_tsc = 0;
        const uint64_t timer_interval =
            rte_get_timer_hz() * TIMER_MANAGE_INTERVAL_MS / 1000;

        LOG_INFO("packet worker started on lcore %u", rte_lcore_id());
        while (1) {
                struct rte_mbuf *mbufs[BURST_SIZE];
                socket_owner_process_commands();

                unsigned int nb_rx = rte_ring_mc_dequeue_burst(
                    ring->in, (void **)mbufs, BURST_SIZE, NULL);
                for (unsigned int i = 0; i < nb_rx; i++)
                        dispatch_packet(mp, mbufs[i], ring->out);

                socket_owner_process_commands();

                uint64_t now = rte_get_timer_cycles();
                if (now - last_timer_tsc >= timer_interval) {
                        rte_timer_manage();
                        last_timer_tsc = now;
                }

                if (g_reactor != NULL)
                        g_reactor(g_reactor_ctx, BURST_SIZE);

                for (struct nsock *sk = g_sock_list; sk != NULL;
                     sk = sk->next) {
                        if (sk->ops->tx_flush != NULL)
                                sk->ops->tx_flush(sk, mp);
                }
        }
}
