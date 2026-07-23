/**
 * @file netarch.c
 * @brief Entry point: EAL setup, the rx/tx I/O loop, the worker that
 *        dispatches packets to the protocol modules, and the ARP sweep timer.
 *
 * Data flow:
 *   NIC --rx--> in_ring --> pkt_worker --> {arp,udp,icmp}_handle --> out_ring
 *       --tx--> NIC
 */
#include "arp.h"
#include "config.h"
#include "icmp.h"
#include "log.h"
#include "net_context.h"
#include "port.h"
#include "ring.h"
#include "socket_api.h"
#include "udp.h"

#include <netinet/in.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_launch.h>
#include <rte_lcore.h>
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
                rte_pktmbuf_free(mbuf);
                return;
        }

        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);

        switch (ip->next_proto_id) {
#if ENABLE_UDP_ECHO
        case IPPROTO_UDP:
                udp_handle(mp, mbuf);
                return;
#endif
#if ENABLE_ICMP
        case IPPROTO_ICMP:
                icmp_handle(mp, mbuf, out);
                return;
#endif
        default:
                LOG_DEBUG("dropping IPv4 proto %u", ip->next_proto_id);
                rte_pktmbuf_free(mbuf);
                return;
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
                unsigned int nb_rx = rte_ring_mc_dequeue_burst(
                    ring->in, (void **)mbufs, BURST_SIZE, NULL);

                for (unsigned int i = 0; i < nb_rx; i++)
                        dispatch_packet(mp, mbufs[i], ring->out);

#if ENABLE_UDP_APP
                udp_out(mp);
#endif
        }

        return 0;
}

#define UDP_APP_RECV_BUFFER_SIZE 128
/**
 * @brief
 *
 */
static int udp_server_entry(__attribute__((unused)) void *arg) {

        int connfd = nsocket(AF_INET, SOCK_DGRAM, 0);
        if (connfd == -1) {
                printf("sockfd failed\n");
                return -1;
        }

        struct sockaddr_in localaddr, clientaddr; // struct sockaddr
        memset(&localaddr, 0, sizeof(struct sockaddr_in));

        localaddr.sin_port = htons(8889);
        localaddr.sin_family = AF_INET;
        localaddr.sin_addr.s_addr = inet_addr("192.168.21.2"); // 0.0.0.0

        if (nbind(connfd, (struct sockaddr *)&localaddr, sizeof(localaddr)) <
            0) {
                LOG_ERROR("nbind failed");
                return -1;
        }

        LOG_INFO("UDP server listening on " IP_FMT ":%u",
                 IP_ARG(g_net.local_ip), rte_be_to_cpu_16(localaddr.sin_port));

        char buffer[UDP_APP_RECV_BUFFER_SIZE] = {0};
        socklen_t addrlen = sizeof(clientaddr);
        while (1) {

                if (nrecvfrom(connfd, buffer, UDP_APP_RECV_BUFFER_SIZE, 0,
                              (struct sockaddr *)&clientaddr, &addrlen) < 0) {

                        continue;

                } else {

                        printf("recv from %s:%d, data:%s\n",
                               inet_ntoa(clientaddr.sin_addr),
                               ntohs(clientaddr.sin_port), buffer);
                        if (nsendto(connfd, buffer, strlen(buffer), 0,
                                    (struct sockaddr *)&clientaddr,
                                    sizeof(clientaddr)) < 0) {
                                LOG_ERROR("nsendto failed");
                        }
                }
        }

        nclose(connfd);
}

#if ENABLE_ARP_SWEEP
/**
 * @brief Timer callback: broadcast/known ARP requests across the /24 subnet.
 */
static void arp_sweep_cb(__attribute__((unused)) struct rte_timer *timer,
                         void *arg) {
        static const uint8_t broadcast_mac[RTE_ETHER_ADDR_LEN] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        struct rte_mempool *mp = (struct rte_mempool *)arg;
        struct inout_ring *ring = ring_instance();

        for (int i = 1; i <= 254; i++) {
                uint32_t dst_ip =
                    (g_net.local_ip & 0x00FFFFFF) | (0xFF000000 & (i << 24));

                uint8_t *known = arp_lookup(dst_ip);
                const uint8_t *dst_mac = known ? known : broadcast_mac;

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

#if ENABLE_ARP_SWEEP
        rte_timer_subsystem_init();
        struct rte_timer arp_timer;
        rte_timer_init(&arp_timer);
        uint64_t hz = rte_get_timer_hz();
        unsigned int lcore_id = rte_lcore_id();
        rte_timer_reset(&arp_timer, hz, PERIODICAL, lcore_id, arp_sweep_cb, mp);
#endif

        unsigned int main_lcore = rte_lcore_id();
        unsigned int worker_lcore = rte_get_next_lcore(main_lcore, 1, 0);
        if (worker_lcore == RTE_MAX_LCORE)
                rte_exit(EXIT_FAILURE,
                         "need at least 2 lcores (e.g. -l 0-1), got only "
                         "main lcore %u\n",
                         main_lcore);

#if ENABLE_UDP_APP
        unsigned int app_lcore = rte_get_next_lcore(worker_lcore, 1, 0);
        if (app_lcore == RTE_MAX_LCORE)
                rte_exit(EXIT_FAILURE,
                         "ENABLE_UDP_APP needs 3 lcores (e.g. -l 0-2), "
                         "main=%u worker=%u\n",
                         main_lcore, worker_lcore);

        if (rte_eal_remote_launch(udp_server_entry, mp, app_lcore) < 0)
                rte_exit(EXIT_FAILURE,
                         "failed to launch udp_server on lcore %u\n",
                         app_lcore);
        LOG_INFO("udp_server scheduled on lcore %u", app_lcore);
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

#if ENABLE_ARP_SWEEP
                static uint64_t last_tsc = 0;
                uint64_t cur_tsc = rte_rdtsc();
                if (cur_tsc - last_tsc > TIMER_RESOLUTION_CYCLES) {
                        rte_timer_manage();
                        last_tsc = cur_tsc;
                }
#endif
        }

        return 0;
}
