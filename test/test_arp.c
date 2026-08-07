#include "../pro-stack/arp.h"
#include "../pro-stack/config.h"
#include "../pro-stack/net_context.h"

#include <assert.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <stdio.h>
#include <string.h>

static uint64_t cycles_after_ms(uint64_t now, uint64_t milliseconds) {
        return now + rte_get_timer_hz() * milliseconds / 1000U + 1U;
}

static void drain_ring(struct rte_ring *ring) {
        struct rte_mbuf *mbuf;

        while (rte_ring_sc_dequeue(ring, (void **)&mbuf) == 0)
                rte_pktmbuf_free(mbuf);
}

int main(int argc, char **argv) {
        int eal_args = rte_eal_init(argc, argv);
        assert(eal_args >= 0);

        struct rte_mempool *pool =
            rte_pktmbuf_pool_create("arp_test_pool", 1024, 0, 0,
                                    RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        assert(pool != NULL);
        struct rte_ring *out =
            rte_ring_create("arp_test_out", 64, rte_socket_id(), 0);
        assert(out != NULL);

        const uint32_t target_ip = 0x0a000001U;
        const uint8_t first_mac[RTE_ETHER_ADDR_LEN] = {0x02, 0x00, 0x00,
                                                       0x00, 0x00, 0x01};
        const uint8_t second_mac[RTE_ETHER_ADDR_LEN] = {0x02, 0x00, 0x00,
                                                        0x00, 0x00, 0x02};

        g_net.local_ip = 0x0a000002U;
        const uint8_t local_mac[RTE_ETHER_ADDR_LEN] = {0x02, 0x00, 0x00,
                                                       0x00, 0x00, 0xfe};
        memcpy(g_net.local_mac, local_mac, sizeof(local_mac));

        struct arp_table *table = arp_table_instance();
        uint64_t now = rte_get_timer_cycles();

        assert(arp_resolve(pool, out, target_ip, now) == NULL);
        assert(rte_ring_count(out) == 1);
        assert(table->count == 1);

        assert(arp_resolve(pool, out, target_ip, now) == NULL);
        assert(rte_ring_count(out) == 1);

        now = cycles_after_ms(now, ARP_PROBE_INTERVAL_MS);
        assert(arp_resolve(pool, out, target_ip, now) == NULL);
        assert(rte_ring_count(out) == 2);
        drain_ring(out);

        now = cycles_after_ms(now, ARP_PROBE_INTERVAL_MS);
        assert(arp_resolve(pool, out, target_ip, now) == NULL);
        assert(rte_ring_count(out) == 1);
        now = cycles_after_ms(now, ARP_PROBE_INTERVAL_MS);
        assert(arp_resolve(pool, out, target_ip, now) == NULL);
        assert(rte_ring_count(out) == 1);
        now = cycles_after_ms(now, ARP_FAILED_TTL_MS);
        assert(arp_resolve(pool, out, target_ip, now) == NULL);
        assert(rte_ring_count(out) == 2);
        drain_ring(out);

        arp_table_learn(target_ip, first_mac);
        const uint8_t *resolved =
            arp_resolve(pool, out, target_ip, rte_get_timer_cycles());
        assert(resolved != NULL);
        assert(memcmp(resolved, first_mac, RTE_ETHER_ADDR_LEN) == 0);

        arp_table_learn(target_ip, second_mac);
        resolved = arp_resolve(pool, out, target_ip, rte_get_timer_cycles());
        assert(resolved != NULL);
        assert(memcmp(resolved, second_mac, RTE_ETHER_ADDR_LEN) == 0);

        uint32_t before_expiry = table->count;
        now = cycles_after_ms(rte_get_timer_cycles(), ARP_REACHABLE_TTL_MS);
        arp_maintain(now);
        assert(table->count == before_expiry - 1);

        for (uint32_t i = 0; i < ARP_CACHE_CAPACITY; i++) {
                uint32_t ip = 0x0a010000U + i;
                uint8_t mac[RTE_ETHER_ADDR_LEN] = {
                    0x02, 0x00, 0x00, 0x01, (uint8_t)(i >> 8), (uint8_t)i};
                arp_table_learn(ip, mac);
        }
        assert(table->count == ARP_CACHE_CAPACITY);

        const uint32_t evicting_ip = 0x0a020001U;
        arp_table_learn(evicting_ip, second_mac);
        assert(table->count == ARP_CACHE_CAPACITY);
        resolved = arp_resolve(pool, out, evicting_ip, rte_get_timer_cycles());
        assert(resolved != NULL);
        assert(memcmp(resolved, second_mac, RTE_ETHER_ADDR_LEN) == 0);

        drain_ring(out);
        rte_ring_free(out);
        rte_mempool_free(pool);
        printf("ARP cache tests passed\n");
        return 0;
}
