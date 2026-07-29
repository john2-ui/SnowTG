/**
 * @file main.c
 * @brief Entry point: EAL setup, the rx/tx I/O loop, the worker that
 *        dispatches packets to the protocol modules, and the ARP sweep timer.
 *
 * Data flow:
 *   main lcore: NIC RX -> in ring; out ring -> NIC TX
 *   worker:    in ring -> protocol handlers; socket send rings -> out ring
 *   UDP/TCP apps: socket rings <-> echo server and/or active-open client
 */
#include "arp.h"
#include "config.h"
#include "icmp.h"
#include "log.h"
#include "net_context.h"
#include "port.h"
#include "ring.h"
#include "socket.h"
#include "tcp_app.h"
#include "udp_app.h"

#include <generic/rte_cycles.h>
#include <netinet/in.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#if ENABLE_PDUMP
#include <rte_pdump.h>
#endif
#include <rte_timer.h>

/** Local IPv4 address advertised by this stack (network byte order). */
static uint32_t g_local_ip = MAKE_IPV4_ADDR(192, 168, 21, 2);

/**
 * @brief Route one inbound frame to the matching protocol handler.
 *
 * Each handler is responsible for freeing @p mbuf; unhandled frames are freed
 * here.
 */
static void dispatch_packet(struct rte_mempool *mp, struct rte_mbuf *mbuf,
                            struct rte_ring *out) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);

#if ENABLE_ARP
        if (eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
                arp_handle(mp, mbuf, out);
                return;
        }
#endif

        if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                LOG_DEBUG("dropping non-IPv4 frame (ethertype=0x%04x)",
                          rte_be_to_cpu_16(eth->ether_type));
                /* TODO: support IPv6 (RTE_ETHER_TYPE_IPV6) and ARP beyond IPv4.
                 * Only ARP and IPv4 are handled today. */
                rte_pktmbuf_free(mbuf);
                return;
        }

        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
        /* TODO: IP fragment reassembly. Fragmented IPv4 datagrams (non-zero
         * fragment_offset / MF flag) are passed straight to the L4 handler,
         * which only sees the first fragment; the rest are misparsed. */

        switch (ip->next_proto_id) {
#if ENABLE_ICMP
        case IPPROTO_ICMP:
                icmp_handle(mp, mbuf, out);
                return;
#endif
        default: {
                const struct sock_ops *ops = sock_ops_lookup(ip->next_proto_id);
                if (ops != NULL && ops->ingress != NULL) {
                        ops->ingress(mbuf); /* ingress consumes mbuf */
                        return;
                }
                LOG_DEBUG("dropping IPv4 proto %u", ip->next_proto_id);
                rte_pktmbuf_free(mbuf);
                return;
        }
        }
}

/**
 * @brief Worker lcore: drain the in ring and dispatch each packet.
 */
static int pkt_worker(void *arg) {
        struct rte_mempool *mp = (struct rte_mempool *)arg;
        struct inout_ring *ring = ring_instance();

        LOG_INFO("packet worker started on lcore %u", rte_lcore_id());

        while (1) {
                struct rte_mbuf *mbufs[BURST_SIZE];
                /*
                 * Apply app receive progress before packet handling, so newly
                 * freed buffer space can drain OFO data and advertise an
                 * updated TCP receive window without waiting for inbound I/O.
                 */
                tcp_process_app_events();
                unsigned int nb_rx = rte_ring_mc_dequeue_burst(
                    ring->in, (void **)mbufs, BURST_SIZE, NULL);

                for (unsigned int i = 0; i < nb_rx; i++)
                        dispatch_packet(mp, mbufs[i], ring->out);

                /* Handle app reads that raced with packet dispatch. */
                tcp_process_app_events();

                /*
                 * Drain each socket's send ring toward the NIC. Iterating the
                 * unified socket list means any transport registered via
                 * sock_ops is flushed here without per-protocol calls.
                 */
                for (struct nsock *sk = g_sock_list; sk != NULL;
                     sk = sk->next) {
                        if (sk->ops->tx_flush != NULL)
                                sk->ops->tx_flush(sk, mp);
                }
        }

        return 0;
}

#if ENABLE_ARP_SWEEP
/**
 * @brief Timer callback: broadcast/known ARP requests across the /24 subnet.
 */
static void arp_sweep_cb(__attribute__((unused)) struct rte_timer *timer,
                         void *arg) {
        struct rte_mempool *mp = (struct rte_mempool *)arg;
        struct inout_ring *ring = ring_instance();

        for (int i = 1; i <= 254; i++) {
                uint32_t dst_ip =
                    (g_net.local_ip & 0x00FFFFFF) | (0xFF000000 & (i << 24));

                uint8_t *known = arp_lookup(dst_ip);
                const uint8_t *dst_mac = known ? known : g_broadcast_mac;

                struct rte_mbuf *arp = arp_build_pkt(
                    mp, RTE_ARP_OP_REQUEST, dst_mac, g_net.local_ip, dst_ip);
                if (arp != NULL)
                        rte_ring_mp_enqueue_burst(ring->out, (void **)&arp, 1,
                                                  NULL);
        }

        LOG_DEBUG("arp sweep enqueued for subnet " IP_FMT "/24",
                  IP_ARG(g_net.local_ip & 0x00FFFFFF));
}
#endif

int main(int argc, char *argv[]) {
        if (rte_eal_init(argc, argv) < 0)
                rte_exit(EXIT_FAILURE, "rte_eal_init() failed\n");

#if ENABLE_PDUMP
        /* Register the capture callbacks so dpdk-pdump (a secondary process)
         * can attach to our ports and mirror traffic to a pcap file. */
        if (rte_pdump_init() < 0)
                rte_exit(EXIT_FAILURE, "rte_pdump_init() failed\n");
#endif

        struct rte_mempool *mp =
            rte_pktmbuf_pool_create("mbuf_pool", NUM_MBUFS, 0, 0,
                                    RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (mp == NULL)
                rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create() failed\n");

        net_context_set_mempool(mp);

        const uint16_t port_id = 0;
        port_init(port_id, mp);
        net_context_init(port_id, g_local_ip);

        /*
         * Create the shared singletons on the main lcore *before* launching the
         * worker. Both threads use these, and lazy creation from two lcores at
         * once races (a half-initialized ring can be observed), which crashes
         * as soon as the bridged NIC delivers its first broadcast frame.
         */
        struct inout_ring *ring = ring_instance();
        arp_table_instance();

        /*
         * Timers (ARP sweep + TCP SYN RTO) are always managed on this lcore.
         * rte_timer_manage() runs in the main I/O loop below; TCP arms its
         * SINGLE timers with rte_get_main_lcore() so callbacks land here.
         */
        rte_timer_subsystem_init();
        uint64_t hz = rte_get_timer_hz();
        uint64_t timer_resolution_cycles = hz * TIMER_MANAGE_INTERVAL_MS / 1000;
        unsigned int timer_lcore = rte_lcore_id();

#if ENABLE_ARP_SWEEP
        struct rte_timer arp_timer;
        rte_timer_init(&arp_timer);
        rte_timer_reset(&arp_timer, hz * 60, PERIODICAL, timer_lcore,
                        arp_sweep_cb, mp); /* period: 60 seconds */
#endif

        unsigned int main_lcore = rte_lcore_id();
        unsigned int worker_lcore = rte_get_next_lcore(main_lcore, 1, 0);
        if (worker_lcore == RTE_MAX_LCORE)
                rte_exit(EXIT_FAILURE,
                         "need at least 2 lcores (e.g. -l 0-1), got only "
                         "main lcore %u\n",
                         main_lcore);

        unsigned int next_app_lcore = worker_lcore;

#if ENABLE_UDP_APP
        next_app_lcore = rte_get_next_lcore(next_app_lcore, 1, 0);
        if (next_app_lcore == RTE_MAX_LCORE)
                rte_exit(EXIT_FAILURE,
                         "ENABLE_UDP_APP needs an extra lcore after worker "
                         "(e.g. -l 0-2), main=%u worker=%u\n",
                         main_lcore, worker_lcore);

        if (rte_eal_remote_launch(udp_app_entry, mp, next_app_lcore) < 0)
                rte_exit(EXIT_FAILURE,
                         "failed to launch udp_server on lcore %u\n",
                         next_app_lcore);
        LOG_INFO("udp_server scheduled on lcore %u", next_app_lcore);
#endif

#if ENABLE_TCP_APP && ENABLE_TCP_SERVER
        next_app_lcore = rte_get_next_lcore(next_app_lcore, 1, 0);
        if (next_app_lcore == RTE_MAX_LCORE)
                rte_exit(EXIT_FAILURE,
                         "ENABLE_TCP_SERVER needs an extra lcore after "
                         "worker/UDP app (e.g. -l 0-3), main=%u worker=%u\n",
                         main_lcore, worker_lcore);

        if (rte_eal_remote_launch(tcp_server_entry, mp, next_app_lcore) < 0)
                rte_exit(EXIT_FAILURE,
                         "failed to launch tcp_server on lcore %u\n",
                         next_app_lcore);
        LOG_INFO("tcp_server scheduled on lcore %u", next_app_lcore);
#endif

#if ENABLE_TCP_APP && ENABLE_TCP_CLIENT
        next_app_lcore = rte_get_next_lcore(next_app_lcore, 1, 0);
        if (next_app_lcore == RTE_MAX_LCORE)
                rte_exit(EXIT_FAILURE,
                         "ENABLE_TCP_CLIENT needs an extra lcore after "
                         "worker/UDP/TCP server (e.g. -l 0-4), main=%u "
                         "worker=%u\n",
                         main_lcore, worker_lcore);

        if (rte_eal_remote_launch(tcp_client_entry, mp, next_app_lcore) < 0)
                rte_exit(EXIT_FAILURE,
                         "failed to launch tcp_client on lcore %u\n",
                         next_app_lcore);
        LOG_INFO("tcp_client scheduled on lcore %u", next_app_lcore);
#endif

        if (rte_eal_remote_launch(pkt_worker, mp, worker_lcore) < 0)
                rte_exit(EXIT_FAILURE,
                         "failed to launch pkt_worker on lcore %u\n",
                         worker_lcore);
        LOG_INFO("pkt_worker scheduled on lcore %u", worker_lcore);

        while (1) {
                /* Receive from the NIC and push to the worker. */
                struct rte_mbuf *rx[BURST_SIZE];
                unsigned int nb_rx =
                    rte_eth_rx_burst(g_net.port_id, 0, rx, BURST_SIZE);
                if (nb_rx > 0) {
                        LOG_DEBUG("rx burst %u packets from NIC", nb_rx);
                        unsigned int enq = rte_ring_sp_enqueue_burst(
                            ring->in, (void **)rx, nb_rx, NULL);
                        if (enq < nb_rx) {
                                LOG_WARN("in ring full, dropped %u packets",
                                         nb_rx - enq);
                                for (unsigned int i = enq; i < nb_rx; i++)
                                        rte_pktmbuf_free(rx[i]);
                        }
                }

                /* Transmit whatever the worker produced. */
                struct rte_mbuf *tx[BURST_SIZE];
                unsigned int nb_tx = rte_ring_sc_dequeue_burst(
                    ring->out, (void **)tx, BURST_SIZE, NULL);
                if (nb_tx > 0) {
                        unsigned int nb_sent =
                            rte_eth_tx_burst(g_net.port_id, 0, tx, nb_tx);
                        /* Free only the packets the driver did not take. */
                        for (unsigned int i = nb_sent; i < nb_tx; i++)
                                rte_pktmbuf_free(tx[i]);
                }

                static uint64_t last_tsc = 0;
                uint64_t cur_tsc = rte_get_timer_cycles();
                if (cur_tsc - last_tsc > timer_resolution_cycles) {
                        rte_timer_manage();
                        last_tsc = cur_tsc;
                }
        }

        return 0;
}
