#include "../pro-stack/arp.h"
#include "../pro-stack/config.h"
#include "../pro-stack/ipv4_reassembly.h"
#include "../pro-stack/net_context.h"
#include "../pro-stack/owner_io.h"
#include "../pro-stack/ring.h"
#include "../pro-stack/rx_dispatch.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/socket_owner_internal.h"
#include "../pro-stack/udp.h"

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_udp.h>
#include <stdint.h>
#include <string.h>

static const uint8_t test_mac[RTE_ETHER_ADDR_LEN] = {0x02, 0, 0, 0, 0, 1};

static struct rte_mbuf *build_fragment(struct rte_mempool *mp,
                                       const struct rte_mbuf *whole,
                                       uint16_t packet_id, uint32_t offset,
                                       uint16_t payload_len, int more) {
        const uint16_t l2_len = sizeof(struct rte_ether_hdr);
        const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
        const uint16_t frame_len = l2_len + l3_len + payload_len;
        struct rte_mbuf *fragment = rte_pktmbuf_alloc(mp);
        uint8_t *dst;
        const void *source;

        assert(fragment != NULL);
        dst = (uint8_t *)rte_pktmbuf_append(fragment, frame_len);
        assert(dst != NULL);
        source = rte_pktmbuf_read(whole, 0, l2_len + l3_len, dst);
        assert(source != NULL);
        if (source != dst)
                rte_memcpy(dst, source, l2_len + l3_len);
        source = rte_pktmbuf_read(whole, l2_len + l3_len + offset, payload_len,
                                  dst + l2_len + l3_len);
        assert(source != NULL);
        if (source != dst + l2_len + l3_len)
                rte_memcpy(dst + l2_len + l3_len, source, payload_len);

        struct rte_ipv4_hdr *ip =
            rte_pktmbuf_mtod_offset(fragment, struct rte_ipv4_hdr *, l2_len);
        uint16_t fragment_field =
            (uint16_t)(offset / RTE_IPV4_HDR_OFFSET_UNITS);
        if (more)
                fragment_field |= RTE_IPV4_HDR_MF_FLAG;
        ip->packet_id = rte_cpu_to_be_16(packet_id);
        ip->fragment_offset = rte_cpu_to_be_16(fragment_field);
        ip->total_length = rte_cpu_to_be_16(l3_len + payload_len);
        ip->hdr_checksum = 0;
        ip->hdr_checksum = rte_ipv4_cksum(ip);
        return fragment;
}

static struct rte_mbuf *build_udp(struct rte_mempool *mp, uint32_t source_ip,
                                  uint32_t dest_ip, uint16_t source_port,
                                  uint16_t dest_port, const uint8_t *payload,
                                  uint16_t payload_len) {
        struct rte_mbuf *mbuf =
            udp_build_pkt(mp, test_mac, source_ip, dest_ip, source_port,
                          dest_port, payload, payload_len);
        struct rte_ether_hdr *eth;

        assert(mbuf != NULL);
        eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        rte_memcpy(eth->src_addr.addr_bytes, test_mac, sizeof(test_mac));
        return mbuf;
}

static void assert_reassembled_udp(struct rte_mbuf *mbuf,
                                   const uint8_t *payload,
                                   uint16_t payload_len) {
        const uint16_t l2_len = sizeof(struct rte_ether_hdr);
        const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
        struct rte_ipv4_hdr *ip =
            rte_pktmbuf_mtod_offset(mbuf, struct rte_ipv4_hdr *, l2_len);
        struct rte_udp_hdr udp;
        const struct rte_udp_hdr *udp_read;
        uint8_t copy[256];

        assert(mbuf->nb_segs > 1);
        assert(rte_pktmbuf_pkt_len(mbuf) ==
               l2_len + l3_len + sizeof(udp) + payload_len);
        assert(ip->fragment_offset == 0);
        assert(rte_ipv4_cksum(ip) == 0);
        assert(rte_ipv4_udptcp_cksum_mbuf_verify(mbuf, ip, l2_len + l3_len) ==
               0);
        udp_read = rte_pktmbuf_read(mbuf, l2_len + l3_len, sizeof(udp), &udp);
        assert(udp_read != NULL);
        assert(rte_be_to_cpu_16(udp_read->dgram_len) ==
               sizeof(udp) + payload_len);
        assert(payload_len <= sizeof(copy));
        assert(rte_pktmbuf_read(mbuf, l2_len + l3_len + sizeof(udp),
                                payload_len, copy) != NULL);
        assert(memcmp(copy, payload, payload_len) == 0);
}

static void test_order_duplicate_and_dispatch(struct ipv4_reassembly *ctx,
                                              struct rte_mempool *mp,
                                              uint32_t source_ip,
                                              uint32_t dest_ip) {
        enum { FIRST_LEN = 80, PAYLOAD_LEN = 200 };
        uint8_t payload[PAYLOAD_LEN];
        struct rte_mbuf *whole;
        struct rte_mbuf *first;
        struct rte_mbuf *last;
        struct rte_mbuf *duplicate;
        struct rte_mbuf *result;
        uint64_t now = rte_get_timer_cycles();
        unsigned int available = rte_mempool_avail_count(mp);
        unsigned int workers[] = {11, 22};
        struct rx_dispatch_result dispatch;
        const uint16_t dest_port = rte_cpu_to_be_16(9000);

        for (unsigned int i = 0; i < sizeof(payload); i++)
                payload[i] = (uint8_t)i;
        whole = build_udp(mp, source_ip, dest_ip, rte_cpu_to_be_16(1000),
                          dest_port, payload, sizeof(payload));
        first = build_fragment(mp, whole, 1, 0, FIRST_LEN, 1);
        last = build_fragment(
            mp, whole, 1, FIRST_LEN,
            sizeof(struct rte_udp_hdr) + PAYLOAD_LEN - FIRST_LEN, 0);
        duplicate = build_fragment(
            mp, whole, 1, FIRST_LEN,
            sizeof(struct rte_udp_hdr) + PAYLOAD_LEN - FIRST_LEN, 0);
        rte_pktmbuf_free(whole);

        assert(ipv4_reassembly_process(ctx, last, now) == NULL);
        assert(ipv4_reassembly_process(ctx, duplicate, now + 1) == NULL);
        result = ipv4_reassembly_process(ctx, first, now + 2);
        assert(result != NULL);
        assert_reassembled_udp(result, payload, sizeof(payload));

        assert(rx_dispatch_configure_workers(workers, 2) == 0);
        assert(rx_dispatch_register_endpoint(IPPROTO_UDP, dest_ip, dest_port,
                                             workers[1]) == 0);
        rx_dispatch_classify(result, 0, &dispatch);
        assert(dispatch.action == RX_DISPATCH_DELIVER);
        assert(dispatch.owner_hit);
        assert(dispatch.worker_index == 1);
        rx_dispatch_unregister_endpoint(IPPROTO_UDP, dest_ip, dest_port,
                                        workers[1]);
        rx_dispatch_reset();

        rte_pktmbuf_free(result);
        assert(rte_mempool_avail_count(mp) == available);
}

static void test_overlap_and_validation(struct ipv4_reassembly *ctx,
                                        struct rte_mempool *mp,
                                        uint32_t source_ip, uint32_t dest_ip) {
        uint8_t payload[128] = {0};
        struct rte_mbuf *whole =
            build_udp(mp, source_ip, dest_ip, rte_cpu_to_be_16(1001),
                      rte_cpu_to_be_16(9001), payload, sizeof(payload));
        unsigned int available = rte_mempool_avail_count(mp) + 1;
        uint64_t now = rte_get_timer_cycles();
        struct rte_mbuf *fragment;

        fragment = build_fragment(mp, whole, 2, 0, 80, 1);
        assert(ipv4_reassembly_process(ctx, fragment, now) == NULL);
        fragment = build_fragment(mp, whole, 2, 64, 72, 0);
        assert(ipv4_reassembly_process(ctx, fragment, now + 1) == NULL);
        assert(rte_mempool_avail_count(mp) == available - 1);

        fragment = build_fragment(mp, whole, 3, 0, 10, 1);
        assert(ipv4_reassembly_process(ctx, fragment, now + 2) == NULL);
        assert(rte_mempool_avail_count(mp) == available - 1);

        fragment = build_fragment(mp, whole, 4, 0, 16, 1);
        struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(
            fragment, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
        ip->hdr_checksum ^= rte_cpu_to_be_16(1);
        assert(ipv4_reassembly_process(ctx, fragment, now + 3) == NULL);
        assert(rte_mempool_avail_count(mp) == available - 1);

        fragment = build_fragment(mp, whole, 5, 0, 8, 0);
        ip = rte_pktmbuf_mtod_offset(fragment, struct rte_ipv4_hdr *,
                                     sizeof(struct rte_ether_hdr));
        ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_OFFSET_MASK);
        ip->hdr_checksum = 0;
        ip->hdr_checksum = rte_ipv4_cksum(ip);
        assert(ipv4_reassembly_process(ctx, fragment, now + 4) == NULL);
        assert(rte_mempool_avail_count(mp) == available - 1);

        fragment = build_fragment(mp, whole, 5, 0, 8, 0);
        ip = rte_pktmbuf_mtod_offset(fragment, struct rte_ipv4_hdr *,
                                     sizeof(struct rte_ether_hdr));
        ip->fragment_offset = rte_cpu_to_be_16(UINT16_C(1) << 15);
        ip->hdr_checksum = 0;
        ip->hdr_checksum = rte_ipv4_cksum(ip);
        assert(ipv4_reassembly_process(ctx, fragment, now + 5) == NULL);
        assert(rte_mempool_avail_count(mp) == available - 1);
        rte_pktmbuf_free(whole);
        assert(rte_mempool_avail_count(mp) == available);
}

static void test_fragment_limit_and_timeout(struct ipv4_reassembly *ctx,
                                            struct rte_mempool *mp,
                                            uint32_t source_ip,
                                            uint32_t dest_ip) {
        uint8_t payload[64] = {0};
        struct rte_mbuf *whole =
            build_udp(mp, source_ip, dest_ip, rte_cpu_to_be_16(1002),
                      rte_cpu_to_be_16(9002), payload, sizeof(payload));
        unsigned int available = rte_mempool_avail_count(mp) + 1;
        uint64_t now = rte_get_timer_cycles();

        for (unsigned int i = 0; i < UDP_SENDTO_MAX_DATAGRAMS; i++) {
                struct rte_mbuf *fragment =
                    build_fragment(mp, whole, 6, i * 8, 8, 1);
                assert(ipv4_reassembly_process(ctx, fragment, now + i) == NULL);
        }
        assert(rte_mempool_avail_count(mp) == available - 1);

        struct rte_mbuf *held = build_fragment(mp, whole, 7, 0, 16, 1);
        assert(ipv4_reassembly_process(ctx, held, now + 20) == NULL);
        assert(rte_mempool_avail_count(mp) == available - 2);
        ipv4_reassembly_maintain(
            ctx, now + rte_get_timer_hz() *
                           (IPV4_REASSEMBLY_TIMEOUT_MS / 1000U + 1U));
        assert(rte_mempool_avail_count(mp) == available - 1);
        rte_pktmbuf_free(whole);
        assert(rte_mempool_avail_count(mp) == available);
}

static void test_table_full_recovery(struct rte_mempool *mp,
                                     uint32_t source_ip, uint32_t dest_ip) {
        uint8_t payload[8] = {0};
        struct rte_mbuf *whole =
            build_udp(mp, source_ip, dest_ip, rte_cpu_to_be_16(1004),
                      rte_cpu_to_be_16(9004), payload, sizeof(payload));
        struct ipv4_reassembly ctx;
        unsigned int available = rte_mempool_avail_count(mp) + 1;
        uint64_t now = rte_get_timer_cycles();

        assert(ipv4_reassembly_init(&ctx) == 0);
        for (unsigned int i = 0; i < IPV4_REASSEMBLY_MAX_ENTRIES; i++) {
                struct rte_mbuf *fragment = build_fragment(
                    mp, whole, (uint16_t)(1000U + i), 0, 8, 1);
                assert(ipv4_reassembly_process(&ctx, fragment, now) == NULL);
        }
        assert(rte_mempool_avail_count(mp) ==
               available - IPV4_REASSEMBLY_MAX_ENTRIES - 1);

        struct rte_mbuf *extra =
            build_fragment(mp, whole, 3000, 0, 8, 1);
        assert(ipv4_reassembly_process(&ctx, extra, now) == NULL);
        assert(rte_mempool_avail_count(mp) ==
               available - IPV4_REASSEMBLY_MAX_ENTRIES - 1);
        ipv4_reassembly_fini(&ctx);
        rte_pktmbuf_free(whole);
        assert(rte_mempool_avail_count(mp) == available);
}

static void test_udp_ingress_chained(struct ipv4_reassembly *ctx,
                                     struct rte_mempool *mp, uint32_t source_ip,
                                     uint32_t dest_ip) {
        enum { PAYLOAD_LEN = 160, FIRST_LEN = 64 };
        uint8_t payload[PAYLOAD_LEN];
        uint8_t received[PAYLOAD_LEN];
        struct nsock_handle handle;
        struct nsock *sk;
        struct rte_mbuf *whole;
        struct rte_mbuf *first;
        struct rte_mbuf *last;
        struct rte_mbuf *result;
        struct sockaddr_in source = {0};
        socklen_t source_len = sizeof(source);
        unsigned int available = rte_mempool_avail_count(mp);
        uint64_t now = rte_get_timer_cycles();

        for (unsigned int i = 0; i < sizeof(payload); i++)
                payload[i] = (uint8_t)(255U - i);
        assert(owner_io_socket_create_local(IPPROTO_UDP, &handle) == 0);
        assert(owner_io_bind_ephemeral(handle, dest_ip) == 0);
        sk = socket_owner_resolve_local(handle);
        assert(sk != NULL);
        whole = build_udp(mp, source_ip, dest_ip, rte_cpu_to_be_16(1003),
                          sk->local_port, payload, sizeof(payload));
        first = build_fragment(mp, whole, 8, 0, FIRST_LEN, 1);
        last = build_fragment(
            mp, whole, 8, FIRST_LEN,
            sizeof(struct rte_udp_hdr) + PAYLOAD_LEN - FIRST_LEN, 0);
        rte_pktmbuf_free(whole);
        assert(ipv4_reassembly_process(ctx, last, now) == NULL);
        result = ipv4_reassembly_process(ctx, first, now + 1);
        assert(result != NULL && result->nb_segs == 2);
        assert(udp_ingress(result) == 0);
        assert(owner_io_recvfrom(handle, received, sizeof(received),
                                 (struct sockaddr *)&source,
                                 &source_len) == PAYLOAD_LEN);
        assert(memcmp(received, payload, sizeof(payload)) == 0);
        assert(source.sin_addr.s_addr == source_ip);
        assert(owner_io_close(handle) == 0);
        assert(rte_mempool_avail_count(mp) == available);
}

int main(int argc, char **argv) {
        const uint32_t source_ip = rte_cpu_to_be_32(0xc0a81501);
        const uint32_t dest_ip = rte_cpu_to_be_32(0xc0a81502);
        struct ipv4_reassembly reassembly;
        struct rte_mempool *mp;

        assert(rte_eal_init(argc, argv) >= 0);
        mp =
            rte_pktmbuf_pool_create("ipv4_reassembly_test_mp", 4096, 0, 0,
                                    RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        assert(mp != NULL);
        memset(&g_net, 0, sizeof(g_net));
        g_net.local_ip = dest_ip;
        g_net.ipv4_mtu = RTE_ETHER_MTU;
        g_net.mp = mp;
        assert(ipv4_reassembly_init(&reassembly) == 0);

        test_order_duplicate_and_dispatch(&reassembly, mp, source_ip, dest_ip);
        test_overlap_and_validation(&reassembly, mp, source_ip, dest_ip);
        test_fragment_limit_and_timeout(&reassembly, mp, source_ip, dest_ip);
        test_table_full_recovery(mp, source_ip, dest_ip);

        assert(socket_registry_init_owner_with_capacity(rte_lcore_id(), 16) ==
               0);
        assert(socket_owner_init_with_capacity(rte_lcore_id(), 16) == 0);
        assert(ring_init_owner(rte_lcore_id()) == 0);
        assert(arp_table_init_owner(rte_lcore_id()) == 0);
        test_udp_ingress_chained(&reassembly, mp, source_ip, dest_ip);

        ipv4_reassembly_fini(&reassembly);
        arp_table_fini();
        ring_fini();
        socket_owner_fini();
        socket_registry_fini();
        g_net.mp = NULL;
        rte_mempool_free(mp);
        return 0;
}
