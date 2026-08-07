#ifndef PORT_TESTING
#define PORT_TESTING
#endif

#include "../pro-stack/port.h"
#include "../pro-stack/rx_dispatch.h"

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
        do {                                                                   \
                if (!(condition)) {                                           \
                        fprintf(stderr, "check failed: %s (%s:%d)\n",         \
                                #condition, __FILE__, __LINE__);              \
                        return 1;                                              \
                }                                                              \
        } while (0)

static struct rte_mbuf mbuf_for_frame(uint8_t *frame, uint16_t length) {
        struct rte_mbuf mbuf;

        memset(&mbuf, 0, sizeof(mbuf));
        mbuf.buf_addr = frame;
        mbuf.data_off = 0;
        mbuf.data_len = length;
        mbuf.pkt_len = length;
        return mbuf;
}

static uint16_t make_ipv4_frame(uint8_t *frame, uint8_t protocol,
                                uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port) {
        struct rte_ether_hdr *eth = (struct rte_ether_hdr *)frame;
        struct rte_ipv4_hdr *ip =
            (struct rte_ipv4_hdr *)(frame + sizeof(*eth));
        uint16_t l4_len;

        memset(frame, 0, 128);
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
        ip->version_ihl = RTE_IPV4_VHL_DEF;
        ip->time_to_live = 64;
        ip->next_proto_id = protocol;
        ip->src_addr = remote_ip;
        ip->dst_addr = local_ip;
        if (protocol == IPPROTO_TCP) {
                struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip + 1);

                tcp->src_port = remote_port;
                tcp->dst_port = local_port;
                tcp->data_off = (uint8_t)(sizeof(*tcp) / 4U) << 4;
                l4_len = sizeof(*tcp);
        } else if (protocol == IPPROTO_UDP) {
                struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);

                udp->src_port = remote_port;
                udp->dst_port = local_port;
                udp->dgram_len = rte_cpu_to_be_16(sizeof(*udp));
                l4_len = sizeof(*udp);
        } else {
                uint8_t *icmp = (uint8_t *)(ip + 1);

                icmp[0] = 8;
                icmp[1] = 0;
                icmp[4] = 0x12;
                icmp[5] = 0x34;
                l4_len = 8;
        }
        ip->total_length = rte_cpu_to_be_16(sizeof(*ip) + l4_len);
        return sizeof(*eth) + sizeof(*ip) + l4_len;
}

int main(void) {
        const unsigned int workers[] = {0, 1, 2, 3};
        const uint32_t remote_ip = rte_cpu_to_be_32(UINT32_C(0x0a000001));
        const uint32_t local_ip = rte_cpu_to_be_32(UINT32_C(0x0a000002));
        const uint16_t remote_port = rte_cpu_to_be_16(40000);
        const uint16_t local_port = rte_cpu_to_be_16(8080);
        struct rx_dispatch_result first;
        struct rx_dispatch_result second;
        uint8_t frame[128];
        struct rte_mbuf mbuf;
        uint16_t length;

        CHECK(rx_dispatch_configure_workers(workers, 4) == 0);
        CHECK(port_test_configure_software_rss(4) == 0);

        length = make_ipv4_frame(frame, IPPROTO_TCP, remote_ip, local_ip,
                                 remote_port, local_port);
        mbuf = mbuf_for_frame(frame, length);
        rx_dispatch_classify(&mbuf, 0, &first);
        rx_dispatch_classify(&mbuf, 0, &second);
        CHECK(first.action == RX_DISPATCH_DELIVER);
        CHECK(first.software_hash);
        CHECK(!first.owner_hit);
        CHECK(first.worker_index < 4);
        CHECK(first.worker_index == second.worker_index);

        CHECK(rx_dispatch_register_endpoint(IPPROTO_UDP, local_ip, local_port,
                                            2) == 0);
        length = make_ipv4_frame(frame, IPPROTO_UDP, remote_ip, local_ip,
                                 remote_port, local_port);
        mbuf = mbuf_for_frame(frame, length);
        rx_dispatch_classify(&mbuf, 0, &first);
        CHECK(first.owner_hit);
        CHECK(first.worker_index == 2);
        rx_dispatch_unregister_endpoint(IPPROTO_UDP, local_ip, local_port, 2);

        CHECK(rx_dispatch_register_endpoint(IPPROTO_TCP, local_ip, local_port,
                                            1) == 0);
        length = make_ipv4_frame(frame, IPPROTO_TCP, remote_ip, local_ip,
                                 remote_port, local_port);
        mbuf = mbuf_for_frame(frame, length);
        rx_dispatch_classify(&mbuf, 0, &first);
        CHECK(first.owner_hit);
        CHECK(first.worker_index == 1);
        CHECK(rx_dispatch_register_tcp_connection(remote_ip, local_ip,
                                                  remote_port, local_port,
                                                  3) == 0);
        rx_dispatch_classify(&mbuf, 0, &first);
        CHECK(first.owner_hit);
        CHECK(first.worker_index == 3);
        rx_dispatch_unregister_tcp_connection(remote_ip, local_ip, remote_port,
                                              local_port, 3);
        rx_dispatch_unregister_endpoint(IPPROTO_TCP, local_ip, local_port, 1);

        length = make_ipv4_frame(frame, IPPROTO_ICMP, remote_ip, local_ip, 0,
                                 0);
        mbuf = mbuf_for_frame(frame, length);
        rx_dispatch_classify(&mbuf, 0, &first);
        rx_dispatch_classify(&mbuf, 0, &second);
        CHECK(first.software_hash);
        CHECK(first.worker_index == second.worker_index);

        memset(frame, 0, sizeof(frame));
        ((struct rte_ether_hdr *)frame)->ether_type =
            rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
        mbuf = mbuf_for_frame(frame, sizeof(struct rte_ether_hdr));
        rx_dispatch_classify(&mbuf, 0, &first);
        CHECK(first.parse_fallback);
        CHECK(first.worker_index == 0);

        length = make_ipv4_frame(frame, IPPROTO_TCP, remote_ip, local_ip,
                                 remote_port, local_port);
        ((struct rte_ipv4_hdr *)(frame + sizeof(struct rte_ether_hdr)))
            ->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_MF_FLAG);
        mbuf = mbuf_for_frame(frame, length);
        rx_dispatch_classify(&mbuf, 0, &first);
        CHECK(first.parse_fallback);
        CHECK(first.worker_index == 0);

        CHECK(port_test_configure_hardware_rss(
                  4, RTE_ETH_RSS_NONFRAG_IPV4_TCP |
                         RTE_ETH_RSS_NONFRAG_IPV4_UDP) == 0);
        length = make_ipv4_frame(frame, IPPROTO_TCP, remote_ip, local_ip,
                                 remote_port, local_port);
        mbuf = mbuf_for_frame(frame, length);
        rx_dispatch_classify(&mbuf, 2, &first);
        CHECK(!first.owner_hit);
        CHECK(!first.software_hash);
        CHECK(first.worker_index == 2);

        rx_dispatch_reset();
        port_test_reset();
        return 0;
}
