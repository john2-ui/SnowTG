#include "../pro-stack/arp.h"
#include "../pro-stack/config.h"
#include "../pro-stack/net_context.h"
#include "../pro-stack/ring.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/socket_owner_internal.h"
#include "../pro-stack/udp.h"
#include "../traffic-gen/core/flow.h"
#include "../traffic-gen/core/flow_pool.h"

#include <assert.h>
#include <netinet/in.h>
#include <rte_byteorder.h>
#include <rte_eal.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_timer.h>
#include <rte_udp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static unsigned int test_rx_calls;
static size_t test_last_rx_len;
static const uint8_t test_peer_mac[RTE_ETHER_ADDR_LEN] = {0x02, 0x00, 0x00,
                                                          0x00, 0x00, 0x01};

static enum tg_proto_result
test_proto_on_rx(__attribute__((unused)) struct tg_txn *txn,
                 __attribute__((unused)) const uint8_t *data, size_t len) {
        test_rx_calls++;
        test_last_rx_len = len;
        return TG_PROTO_COMPLETE;
}

static const struct tg_proto_ops test_proto = {
    .name = "flow-udp-test",
    .on_rx = test_proto_on_rx,
};

static void mark_test_peer(struct rte_mbuf *mbuf) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);

        rte_memcpy(eth->src_addr.addr_bytes, test_peer_mac, RTE_ETHER_ADDR_LEN);
}

struct finish_context {
        unsigned int calls;
        enum tg_flow_result result;
};

static void test_on_finish(void *ctx,
                           __attribute__((unused)) const struct tg_flow *flow,
                           enum tg_flow_result result) {
        struct finish_context *finish = ctx;

        finish->calls++;
        finish->result = result;
}

static struct tg_flow *only_flow(struct tg_flow_map *map) {
        for (uint32_t id = 0; id < map->capacity; id++) {
                if (map->by_socket_id[id] != NULL)
                        return map->by_socket_id[id];
        }
        return NULL;
}

static void enqueue_response(struct nsock_handle handle, uint32_t source_ip,
                             uint16_t source_port, const uint8_t *data,
                             uint16_t data_len) {
        struct nsock *sk = socket_owner_resolve_local(handle);
        struct rte_mbuf *mbuf;

        assert(sk != NULL);
        mbuf =
            udp_build_pkt(g_net.mp, g_broadcast_mac, source_ip, g_net.local_ip,
                          source_port, sk->local_port, data, data_len);
        assert(mbuf != NULL);
        mark_test_peer(mbuf);
        assert(udp_ingress(mbuf) == 0);
}

static struct tg_flow *start_udp_flow(struct tg_flow_map *map,
                                      struct tg_flow_pool *pool,
                                      const struct sockaddr_in *peer,
                                      struct finish_context *finish) {
        static const uint8_t request[] = {0x01, 0x02, 0x03, 0x04};

        assert(tg_flow_start_udp(map, pool, (const struct sockaddr *)peer,
                                 sizeof(*peer), &test_proto, NULL, request,
                                 sizeof(request), test_on_finish, finish, NULL,
                                 NULL, NULL) == 0);
        return only_flow(map);
}

static void drain_output_ring(void) {
        struct inout_ring *ring = ring_instance();
        struct rte_mbuf *mbuf;

        assert(ring != NULL);
        while (rte_ring_sc_dequeue(ring->out, (void **)&mbuf) == 0)
                rte_pktmbuf_free(mbuf);
}

static void drain_ready_events(void) {
        struct owner_io_event events[32];

        while (owner_io_ready_burst(events, 32) != 0)
                ;
}

static struct nsock_handle create_local_udp(uint32_t local_ip) {
        struct nsock_handle handle;

        assert(owner_io_socket_create_local(IPPROTO_UDP, &handle) == 0);
        assert(owner_io_bind_ephemeral(handle, local_ip) == 0);
        return handle;
}

static void test_local_udp_short_read(uint32_t local_ip, uint32_t peer_ip,
                                      uint16_t peer_port) {
        static const uint8_t first[] = {0x10, 0x11, 0x12};
        static const uint8_t second[] = {0x20};
        struct nsock_handle handle = create_local_udp(local_ip);
        struct nsock *sk = socket_owner_resolve_local(handle);
        struct sockaddr_in source = {0};
        socklen_t source_len = sizeof(source);
        uint8_t one = 0;
        uint8_t rest[sizeof(first)] = {0};

        assert(sk != NULL);
        enqueue_response(handle, peer_ip, peer_port, first, sizeof(first));
        enqueue_response(handle, peer_ip, peer_port, second, sizeof(second));

        assert(owner_io_recvfrom(handle, &one, sizeof(one),
                                 (struct sockaddr *)&source, &source_len) == 1);
        assert(one == first[0]);
        assert(sk->u.udp.rx_current != NULL);
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(sk->u.udp.rx_current, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
        assert(rte_be_to_cpu_16(udp->dgram_len) ==
               sizeof(struct rte_udp_hdr) + sizeof(first));
        assert(memcmp((const uint8_t *)(udp + 1), first, sizeof(first)) == 0);
        assert(sk->u.udp.rx_current_off == sizeof(one));

        source_len = sizeof(source);
        assert(owner_io_recvfrom(handle, rest, sizeof(rest),
                                 (struct sockaddr *)&source, &source_len) == 2);
        assert(rest[0] == first[1]);
        assert(rest[1] == first[2]);
        assert(sk->u.udp.rx_current == NULL);
        assert(sk->u.udp.rx_current_off == 0);
        source_len = sizeof(source);
        assert(owner_io_recvfrom(handle, rest, sizeof(rest),
                                 (struct sockaddr *)&source, &source_len) == 1);
        assert(rest[0] == second[0]);
        assert(owner_io_close(handle) == 0);
}

static void test_local_udp_short_read_close(uint32_t local_ip, uint32_t peer_ip,
                                            uint16_t peer_port) {
        static const uint8_t payload[] = {0x30, 0x31, 0x32};
        struct nsock_handle handle = create_local_udp(local_ip);
        struct rte_mempool *mp = g_net.mp;
        unsigned int available_before = rte_mempool_avail_count(mp);
        unsigned int available_after_enqueue;
        uint8_t first = 0;

        enqueue_response(handle, peer_ip, peer_port, payload, sizeof(payload));
        available_after_enqueue = rte_mempool_avail_count(mp);
        assert(available_after_enqueue < available_before);
        assert(owner_io_recvfrom(handle, &first, sizeof(first), NULL, NULL) ==
               1);
        assert(first == payload[0]);
        assert(owner_io_close(handle) == 0);
        assert(rte_mempool_avail_count(mp) == available_before);
}

static void test_local_udp_rx_drop(uint32_t local_ip, uint32_t peer_ip,
                                   uint16_t peer_port) {
        const uint8_t payload = 0x5a;
        struct nsock_handle handle = create_local_udp(local_ip);
        struct nsock *sk = socket_owner_resolve_local(handle);
        struct rte_mempool *mp = g_net.mp;
        unsigned int available_before = rte_mempool_avail_count(mp);

        assert(sk != NULL);
        for (unsigned int i = 0; i < UDP_RX_QUEUE_LIMIT; i++) {
                struct rte_mbuf *mbuf = udp_build_pkt(
                    mp, g_broadcast_mac, peer_ip, local_ip, peer_port,
                    sk->local_port, &payload, sizeof(payload));
                assert(mbuf != NULL);
                mark_test_peer(mbuf);
                assert(udp_ingress(mbuf) == 0);
        }
        assert(sk->u.udp.rx_queue_count == UDP_RX_QUEUE_LIMIT);

        struct rte_mbuf *extra =
            udp_build_pkt(mp, g_broadcast_mac, peer_ip, local_ip, peer_port,
                          sk->local_port, &payload, sizeof(payload));
        assert(extra != NULL);
        mark_test_peer(extra);
        assert(udp_ingress(extra) == -1);
        assert(rte_mempool_avail_count(mp) ==
               available_before - UDP_RX_QUEUE_LIMIT);

        assert(owner_io_close(handle) == 0);
        assert(rte_mempool_avail_count(mp) == available_before);
}

static void test_local_udp_arp_retry(uint32_t local_ip, uint32_t peer_ip,
                                     uint16_t peer_port,
                                     const uint8_t *peer_mac) {
        const uint8_t payload = 0x01;
        const uint32_t unresolved_ip = rte_cpu_to_be_32(0xc0a81504);
        struct sockaddr_in peer = {
            .sin_family = AF_INET,
            .sin_port = peer_port,
            .sin_addr.s_addr = unresolved_ip,
        };
        struct nsock_handle handle = create_local_udp(local_ip);
        struct nsock *sk = socket_owner_resolve_local(handle);
        struct owner_io_event event = {0};

        assert(sk != NULL);
        assert(owner_io_sendto(handle, &payload, sizeof(payload),
                               (const struct sockaddr *)&peer,
                               sizeof(peer)) == -1);
        assert(errno == EAGAIN);
        assert(sk->tx_arp_waiting);
        drain_output_ring();

        arp_table_learn(unresolved_ip, peer_mac);
        assert(nsock_tx_dirty_drain(NULL, 1) == 1);
        assert(owner_io_ready_burst(&event, 1) == 1);
        assert(event.handle.id == handle.id);
        assert((event.events & OWNER_IO_EV_WRITE) != 0);
        assert(owner_io_close(handle) == 0);

        /* Keep the caller's peer mapping warm for the following TX test. */
        arp_table_learn(peer_ip, peer_mac);
}

static void test_local_udp_tx_ring_full(uint32_t local_ip, uint32_t peer_ip,
                                        uint16_t peer_port) {
        struct inout_ring *ring = ring_instance();
        struct nsock_handle handle = create_local_udp(local_ip);
        struct sockaddr_in peer = {
            .sin_family = AF_INET,
            .sin_port = peer_port,
            .sin_addr.s_addr = peer_ip,
        };
        const uint8_t payload = 0x02;
        assert(ring != NULL);
        unsigned int capacity = rte_ring_get_capacity(ring->out);
        for (unsigned int i = 0; i < capacity; i++) {
                struct rte_mbuf *mbuf = rte_pktmbuf_alloc(g_net.mp);
                assert(mbuf != NULL);
                assert(rte_ring_sp_enqueue(ring->out, mbuf) == 0);
        }

        errno = 0;
        assert(owner_io_sendto(handle, &payload, sizeof(payload),
                               (const struct sockaddr *)&peer,
                               sizeof(peer)) == -1);
        assert(errno == ENOBUFS);
        assert(owner_io_close(handle) == 0);
        drain_output_ring();
}

int main(int argc, char **argv) {
        const uint32_t local_ip = rte_cpu_to_be_32(0xc0a81502);
        const uint32_t peer_ip = rte_cpu_to_be_32(0xc0a81501);
        const uint32_t wrong_ip = rte_cpu_to_be_32(0xc0a81503);
        const uint16_t peer_port = rte_cpu_to_be_16(53);
        struct sockaddr_in peer = {
            .sin_family = AF_INET,
            .sin_port = peer_port,
            .sin_addr.s_addr = peer_ip,
        };
        struct tg_flow_map map;
        struct tg_flow_pool pool;
        struct finish_context finish = {0};
        struct rte_mempool *mp;
        struct tg_flow *flow;
        struct nsock_handle old_handle;
        static const uint8_t response[] = {0xaa, 0xbb};

        assert(rte_eal_init(argc, argv) >= 0);
        rte_timer_subsystem_init();
        assert(socket_registry_init_owner_with_capacity(rte_lcore_id(), 16) ==
               0);
        assert(socket_owner_init_with_capacity(rte_lcore_id(), 16) == 0);
        assert(ring_init_owner(rte_lcore_id()) == 0);
        assert(arp_table_init_owner(rte_lcore_id()) == 0);

        memset(&g_net, 0, sizeof(g_net));
        g_net.local_ip = local_ip;
        mp =
            rte_pktmbuf_pool_create("flow_udp_test_mp", 4096, 0, 0,
                                    RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        assert(mp != NULL);
        g_net.mp = mp;
        arp_table_learn(peer_ip, test_peer_mac);

        struct nsock_handle implicit_udp;
        uint8_t implicit_payload = 0x5a;
        assert(owner_io_socket_create(IPPROTO_UDP, &implicit_udp) == 0);
        assert(owner_io_sendto(implicit_udp, &implicit_payload,
                               sizeof(implicit_payload),
                               (const struct sockaddr *)&peer, sizeof(peer)) ==
               (ssize_t)sizeof(implicit_payload));
        assert(socket_owner_resolve_local(implicit_udp)->local_port != 0);
        assert(owner_io_close(implicit_udp) == 0);

        assert(tg_flow_map_init_with_capacity(&map, rte_lcore_id(), 16) == 0);
        assert(tg_flow_pool_init(&pool, 4) == 0);

        test_rx_calls = 0;
        flow = start_udp_flow(&map, &pool, &peer, &finish);
        assert(flow != NULL);
        old_handle = flow->handle;
        assert(socket_owner_resolve_local(flow->handle)->io_mode ==
               NSOCK_IO_OWNER_LOCAL);
        assert(socket_owner_resolve_local(flow->handle)->recv_buf == NULL);
        assert(socket_owner_resolve_local(flow->handle)->send_buf == NULL);
        assert(flow->state == TG_FLOW_RECEIVING);
        assert(flow->txn.request_offset == flow->txn.request_len);

        enqueue_response(flow->handle, wrong_ip, peer_port, response,
                         sizeof(response));
        tg_flow_on_event(&map, &pool, flow, OWNER_IO_EV_READ);
        assert(only_flow(&map) == flow);
        assert(finish.calls == 0);
        assert(test_rx_calls == 0);

        enqueue_response(flow->handle, peer_ip, peer_port, response,
                         sizeof(response));
        tg_flow_on_event(&map, &pool, flow, OWNER_IO_EV_READ);
        assert(finish.calls == 1);
        assert(finish.result == TG_FLOW_RESULT_SUCCESS);
        assert(tg_flow_map_lookup(&map, old_handle) == NULL);

        test_last_rx_len = SIZE_MAX;
        flow = start_udp_flow(&map, &pool, &peer, &finish);
        assert(flow != NULL);
        old_handle = flow->handle;
        enqueue_response(flow->handle, peer_ip, peer_port, NULL, 0);
        tg_flow_on_event(&map, &pool, flow, OWNER_IO_EV_READ);
        assert(finish.calls == 2);
        assert(finish.result == TG_FLOW_RESULT_SUCCESS);
        assert(test_last_rx_len == 0);
        assert(tg_flow_map_lookup(&map, old_handle) == NULL);

        flow = start_udp_flow(&map, &pool, &peer, &finish);
        assert(flow != NULL);
        old_handle = flow->handle;
        flow->deadline_cycles = 1;
        tg_flow_expire(&map, &pool, 2);
        assert(finish.calls == 3);
        assert(finish.result == TG_FLOW_RESULT_IO_FAILURE);
        assert(tg_flow_map_lookup(&map, old_handle) == NULL);

        flow = start_udp_flow(&map, &pool, &peer, &finish);
        assert(flow != NULL);
        assert(tg_flow_map_lookup(&map, old_handle) == NULL);
        flow->deadline_cycles = 1;
        tg_flow_expire(&map, &pool, 2);
        assert(finish.calls == 4);

        drain_output_ring();
        drain_ready_events();
        test_local_udp_short_read(local_ip, peer_ip, peer_port);
        drain_ready_events();
        test_local_udp_short_read_close(local_ip, peer_ip, peer_port);
        drain_ready_events();
        test_local_udp_rx_drop(local_ip, peer_ip, peer_port);
        drain_ready_events();
        test_local_udp_arp_retry(local_ip, peer_ip, peer_port, test_peer_mac);
        drain_ready_events();
        test_local_udp_tx_ring_full(local_ip, peer_ip, peer_port);
        drain_ready_events();

        tg_flow_pool_fini(&pool);
        tg_flow_map_fini(&map);
        drain_output_ring();
        g_net.mp = NULL;
        arp_table_fini();
        ring_fini();
        rte_mempool_free(mp);
        socket_owner_fini();
        socket_registry_fini();
        return 0;
}
