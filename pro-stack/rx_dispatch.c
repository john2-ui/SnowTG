#include "rx_dispatch.h"

#include "port.h"

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include <errno.h> // IWYU pragma: keep
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RX_ENDPOINT_TABLE_SIZE 16384U
#define RX_FLOW_TABLE_SIZE 65536U
#define RX_DISPATCH_TOMBSTONE UINT64_MAX

struct rx_dispatch_flow_slot {
        atomic_uint_fast64_t addresses;
        atomic_uint_fast64_t ports_protocol_owner;
};

static atomic_uint_fast64_t g_endpoint_entries[RX_ENDPOINT_TABLE_SIZE];
static struct rx_dispatch_flow_slot g_flow_entries[RX_FLOW_TABLE_SIZE];
static unsigned int g_worker_lcores[RTE_MAX_LCORE];
static uint16_t g_worker_count;
static pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t rx_dispatch_mix64(uint64_t value) {
        value ^= value >> 30;
        value *= UINT64_C(0xbf58476d1ce4e5b9);
        value ^= value >> 27;
        value *= UINT64_C(0x94d049bb133111eb);
        return value ^ (value >> 31);
}

static unsigned int rx_dispatch_endpoint_start(uint64_t identity) {
        return (unsigned int)(rx_dispatch_mix64(identity) &
                              (RX_ENDPOINT_TABLE_SIZE - 1U));
}

static unsigned int rx_dispatch_flow_start(uint64_t addresses,
                                           uint64_t ports_protocol) {
        return (unsigned int)(rx_dispatch_mix64(addresses ^ ports_protocol) &
                              (RX_FLOW_TABLE_SIZE - 1U));
}

static int rx_dispatch_worker_index(unsigned int owner_lcore) {
        for (uint16_t index = 0; index < g_worker_count; index++) {
                if (g_worker_lcores[index] == owner_lcore)
                        return index;
        }
        return -1;
}

static uint64_t rx_dispatch_endpoint_identity(uint8_t protocol,
                                              uint32_t local_ip,
                                              uint16_t local_port) {
        return ((uint64_t)local_ip << 32) | ((uint64_t)local_port << 16) |
               ((uint64_t)protocol << 8);
}

static uint64_t rx_dispatch_endpoint_value(uint8_t protocol,
                                           uint32_t local_ip,
                                           uint16_t local_port,
                                           unsigned int owner_lcore) {
        return rx_dispatch_endpoint_identity(protocol, local_ip, local_port) |
               owner_lcore;
}

static int rx_dispatch_register_value(atomic_uint_fast64_t *entries,
                                      uint32_t entry_count, uint64_t value,
                                      uint64_t identity, unsigned int start) {
        unsigned int tombstone = entry_count;

        for (unsigned int probe = 0; probe < entry_count; probe++) {
                unsigned int slot = (start + probe) & (entry_count - 1U);
                uint64_t entry =
                    atomic_load_explicit(&entries[slot], memory_order_acquire);

                if (entry == 0) {
                        atomic_uint_fast64_t *target =
                            &entries[tombstone == entry_count ? slot
                                                              : tombstone];
                        uint64_t expected =
                            tombstone == entry_count ? 0 : RX_DISPATCH_TOMBSTONE;

                        if (atomic_compare_exchange_strong_explicit(
                                target, &expected, value, memory_order_release,
                                memory_order_relaxed))
                                return 0;
                        return -EAGAIN;
                }
                if (entry == RX_DISPATCH_TOMBSTONE) {
                        if (tombstone == entry_count)
                                tombstone = slot;
                        continue;
                }
                if ((entry & ~UINT64_C(0xff)) == identity)
                        return entry == value ? 0 : -EADDRINUSE;
        }
        if (tombstone != entry_count) {
                atomic_uint_fast64_t *target = &entries[tombstone];
                uint64_t expected = RX_DISPATCH_TOMBSTONE;

                if (atomic_compare_exchange_strong_explicit(
                        target, &expected, value, memory_order_release,
                        memory_order_relaxed))
                        return 0;
                return -EAGAIN;
        }
        return -ENOSPC;
}

static void rx_dispatch_unregister_value(atomic_uint_fast64_t *entries,
                                         uint32_t entry_count, uint64_t value,
                                         unsigned int start) {
        for (unsigned int probe = 0; probe < entry_count; probe++) {
                unsigned int slot = (start + probe) & (entry_count - 1U);
                uint64_t entry =
                    atomic_load_explicit(&entries[slot], memory_order_acquire);

                if (entry == 0)
                        return;
                if (entry != value)
                        continue;
                uint64_t expected = entry;
                (void)atomic_compare_exchange_strong_explicit(
                    &entries[slot], &expected, RX_DISPATCH_TOMBSTONE,
                    memory_order_release, memory_order_relaxed);
                return;
        }
}

static int rx_dispatch_lookup_endpoint(uint8_t protocol, uint32_t local_ip,
                                       uint16_t local_port) {
        uint64_t identity =
            rx_dispatch_endpoint_identity(protocol, local_ip, local_port);
        unsigned int start = rx_dispatch_endpoint_start(identity);

        for (unsigned int probe = 0; probe < RX_ENDPOINT_TABLE_SIZE; probe++) {
                unsigned int slot =
                    (start + probe) & (RX_ENDPOINT_TABLE_SIZE - 1U);
                uint64_t entry = atomic_load_explicit(&g_endpoint_entries[slot],
                                                      memory_order_acquire);

                if (entry == 0)
                        return -1;
                if (entry != RX_DISPATCH_TOMBSTONE &&
                    (entry & ~UINT64_C(0xff)) == identity)
                        return rx_dispatch_worker_index((uint8_t)entry);
        }
        return -1;
}

static uint64_t rx_dispatch_flow_addresses(uint32_t remote_ip,
                                           uint32_t local_ip) {
        return ((uint64_t)remote_ip << 32) | local_ip;
}

static uint64_t rx_dispatch_flow_identity(uint8_t protocol,
                                          uint16_t remote_port,
                                          uint16_t local_port) {
        return ((uint64_t)remote_port << 48) | ((uint64_t)local_port << 32) |
               ((uint64_t)protocol << 16);
}

static uint64_t rx_dispatch_flow_value(uint8_t protocol,
                                       uint16_t remote_port,
                                       uint16_t local_port,
                                       unsigned int owner_lcore) {
        return rx_dispatch_flow_identity(protocol, remote_port, local_port) |
               owner_lcore;
}

static int rx_dispatch_lookup_tcp_connection(uint32_t remote_ip,
                                             uint32_t local_ip,
                                             uint16_t remote_port,
                                             uint16_t local_port) {
        uint64_t addresses = rx_dispatch_flow_addresses(remote_ip, local_ip);
        uint64_t identity =
            rx_dispatch_flow_identity(IPPROTO_TCP, remote_port, local_port);
        unsigned int start = rx_dispatch_flow_start(addresses, identity);

        for (unsigned int probe = 0; probe < RX_FLOW_TABLE_SIZE; probe++) {
                unsigned int slot = (start + probe) & (RX_FLOW_TABLE_SIZE - 1U);
                uint64_t value = atomic_load_explicit(
                    &g_flow_entries[slot].ports_protocol_owner,
                    memory_order_acquire);

                if (value == 0)
                        return -1;
                if (value == RX_DISPATCH_TOMBSTONE ||
                    (value & ~UINT64_C(0xffff)) != identity)
                        continue;

                uint64_t entry_addresses = atomic_load_explicit(
                    &g_flow_entries[slot].addresses, memory_order_relaxed);
                uint64_t verify = atomic_load_explicit(
                    &g_flow_entries[slot].ports_protocol_owner,
                    memory_order_acquire);
                if (value == verify && entry_addresses == addresses)
                        return rx_dispatch_worker_index((uint16_t)value);
        }
        return -1;
}

static int rx_dispatch_register_tcp_value(uint64_t addresses, uint64_t value,
                                          uint64_t identity,
                                          unsigned int start) {
        unsigned int tombstone = RX_FLOW_TABLE_SIZE;

        for (unsigned int probe = 0; probe < RX_FLOW_TABLE_SIZE; probe++) {
                unsigned int slot = (start + probe) & (RX_FLOW_TABLE_SIZE - 1U);
                uint64_t entry = atomic_load_explicit(
                    &g_flow_entries[slot].ports_protocol_owner,
                    memory_order_acquire);

                if (entry == 0 || entry == RX_DISPATCH_TOMBSTONE) {
                        if (entry == RX_DISPATCH_TOMBSTONE &&
                            tombstone == RX_FLOW_TABLE_SIZE)
                                tombstone = slot;
                        if (entry != 0)
                                continue;
                        if (tombstone != RX_FLOW_TABLE_SIZE)
                                slot = tombstone;
                        atomic_store_explicit(
                            &g_flow_entries[slot].ports_protocol_owner, 0,
                            memory_order_release);
                        atomic_store_explicit(&g_flow_entries[slot].addresses,
                                              addresses, memory_order_relaxed);
                        atomic_store_explicit(
                            &g_flow_entries[slot].ports_protocol_owner, value,
                            memory_order_release);
                        return 0;
                }
                if ((entry & ~UINT64_C(0xffff)) == identity) {
                        uint64_t entry_addresses = atomic_load_explicit(
                            &g_flow_entries[slot].addresses,
                            memory_order_acquire);
                        if (entry_addresses == addresses)
                                return entry == value ? 0 : -EADDRINUSE;
                }
        }
        if (tombstone != RX_FLOW_TABLE_SIZE) {
                atomic_store_explicit(
                    &g_flow_entries[tombstone].ports_protocol_owner, 0,
                    memory_order_release);
                atomic_store_explicit(&g_flow_entries[tombstone].addresses,
                                      addresses, memory_order_relaxed);
                atomic_store_explicit(
                    &g_flow_entries[tombstone].ports_protocol_owner, value,
                    memory_order_release);
                return 0;
        }
        return -ENOSPC;
}

int rx_dispatch_configure_workers(const unsigned int *lcores,
                                  uint16_t worker_count) {
        if (lcores == NULL || worker_count == 0 ||
            worker_count > RTE_MAX_LCORE)
                return -EINVAL;

        pthread_mutex_lock(&g_registry_lock);
        for (uint16_t index = 0; index < worker_count; index++) {
                if (lcores[index] > UINT16_MAX) {
                        pthread_mutex_unlock(&g_registry_lock);
                        return -EINVAL;
                }
                for (uint16_t other = 0; other < index; other++) {
                        if (lcores[other] == lcores[index]) {
                                pthread_mutex_unlock(&g_registry_lock);
                                return -EINVAL;
                        }
                }
        }
        for (unsigned int index = 0; index < RX_ENDPOINT_TABLE_SIZE; index++)
                atomic_store_explicit(&g_endpoint_entries[index], 0,
                                      memory_order_relaxed);
        for (unsigned int index = 0; index < RX_FLOW_TABLE_SIZE; index++) {
                atomic_store_explicit(
                    &g_flow_entries[index].ports_protocol_owner, 0,
                    memory_order_relaxed);
                atomic_store_explicit(&g_flow_entries[index].addresses, 0,
                                      memory_order_relaxed);
        }
        memcpy(g_worker_lcores, lcores, worker_count * sizeof(*lcores));
        g_worker_count = worker_count;
        pthread_mutex_unlock(&g_registry_lock);
        return 0;
}

void rx_dispatch_reset(void) {
        pthread_mutex_lock(&g_registry_lock);
        g_worker_count = 0;
        memset(g_worker_lcores, 0, sizeof(g_worker_lcores));
        for (unsigned int index = 0; index < RX_ENDPOINT_TABLE_SIZE; index++)
                atomic_store_explicit(&g_endpoint_entries[index], 0,
                                      memory_order_relaxed);
        for (unsigned int index = 0; index < RX_FLOW_TABLE_SIZE; index++) {
                atomic_store_explicit(
                    &g_flow_entries[index].ports_protocol_owner, 0,
                    memory_order_relaxed);
                atomic_store_explicit(&g_flow_entries[index].addresses, 0,
                                      memory_order_relaxed);
        }
        pthread_mutex_unlock(&g_registry_lock);
}

int rx_dispatch_register_endpoint(uint8_t protocol, uint32_t local_ip,
                                  uint16_t local_port,
                                  unsigned int owner_lcore) {
        uint64_t identity;
        uint64_t value;
        int worker;
        int rc;

        if (g_worker_count == 0)
                return 0;
        if ((protocol != IPPROTO_TCP && protocol != IPPROTO_UDP) ||
            local_port == 0 || owner_lcore > UINT8_MAX)
                return -EINVAL;
        worker = rx_dispatch_worker_index(owner_lcore);
        if (worker < 0)
                return -EINVAL;

        identity =
            rx_dispatch_endpoint_identity(protocol, local_ip, local_port);
        value = rx_dispatch_endpoint_value(protocol, local_ip, local_port,
                                           owner_lcore);
        pthread_mutex_lock(&g_registry_lock);
        rc = rx_dispatch_register_value(
            g_endpoint_entries, RX_ENDPOINT_TABLE_SIZE, value, identity,
            rx_dispatch_endpoint_start(identity));
        pthread_mutex_unlock(&g_registry_lock);
        return rc;
}

void rx_dispatch_unregister_endpoint(uint8_t protocol, uint32_t local_ip,
                                     uint16_t local_port,
                                     unsigned int owner_lcore) {
        uint64_t identity;
        uint64_t value;

        if (g_worker_count == 0 || owner_lcore > UINT8_MAX)
                return;
        identity =
            rx_dispatch_endpoint_identity(protocol, local_ip, local_port);
        value = rx_dispatch_endpoint_value(protocol, local_ip, local_port,
                                           owner_lcore);
        pthread_mutex_lock(&g_registry_lock);
        rx_dispatch_unregister_value(g_endpoint_entries, RX_ENDPOINT_TABLE_SIZE,
                                     value, rx_dispatch_endpoint_start(identity));
        pthread_mutex_unlock(&g_registry_lock);
}

bool rx_dispatch_endpoint_is_registered(uint8_t protocol, uint32_t local_ip,
                                        uint16_t local_port) {
        if (g_worker_count == 0)
                return false;
        if (rx_dispatch_lookup_endpoint(protocol, local_ip, local_port) >= 0)
                return true;
        return local_ip != INADDR_ANY &&
               rx_dispatch_lookup_endpoint(protocol, INADDR_ANY, local_port) >=
                   0;
}

int rx_dispatch_register_tcp_connection(uint32_t remote_ip, uint32_t local_ip,
                                        uint16_t remote_port,
                                        uint16_t local_port,
                                        unsigned int owner_lcore) {
        uint64_t addresses;
        uint64_t identity;
        uint64_t value;
        int rc;

        if (g_worker_count == 0)
                return 0;
        if (remote_port == 0 || local_port == 0 || owner_lcore > UINT16_MAX ||
            rx_dispatch_worker_index(owner_lcore) < 0)
                return -EINVAL;
        addresses = rx_dispatch_flow_addresses(remote_ip, local_ip);
        identity =
            rx_dispatch_flow_identity(IPPROTO_TCP, remote_port, local_port);
        value = rx_dispatch_flow_value(IPPROTO_TCP, remote_port, local_port,
                                       owner_lcore);
        pthread_mutex_lock(&g_registry_lock);
        rc = rx_dispatch_register_tcp_value(
            addresses, value, identity, rx_dispatch_flow_start(addresses, identity));
        pthread_mutex_unlock(&g_registry_lock);
        return rc;
}

void rx_dispatch_unregister_tcp_connection(uint32_t remote_ip,
                                           uint32_t local_ip,
                                           uint16_t remote_port,
                                           uint16_t local_port,
                                           unsigned int owner_lcore) {
        uint64_t addresses;
        uint64_t identity;
        uint64_t value;
        unsigned int start;

        if (g_worker_count == 0 || owner_lcore > UINT16_MAX)
                return;
        addresses = rx_dispatch_flow_addresses(remote_ip, local_ip);
        identity =
            rx_dispatch_flow_identity(IPPROTO_TCP, remote_port, local_port);
        value = rx_dispatch_flow_value(IPPROTO_TCP, remote_port, local_port,
                                       owner_lcore);
        start = rx_dispatch_flow_start(addresses, identity);

        pthread_mutex_lock(&g_registry_lock);
        for (unsigned int probe = 0; probe < RX_FLOW_TABLE_SIZE; probe++) {
                unsigned int slot = (start + probe) & (RX_FLOW_TABLE_SIZE - 1U);
                uint64_t entry = atomic_load_explicit(
                    &g_flow_entries[slot].ports_protocol_owner,
                    memory_order_acquire);

                if (entry == 0)
                        break;
                if (entry != value)
                        continue;
                if (atomic_load_explicit(&g_flow_entries[slot].addresses,
                                         memory_order_acquire) != addresses)
                        continue;
                atomic_store_explicit(
                    &g_flow_entries[slot].ports_protocol_owner,
                    RX_DISPATCH_TOMBSTONE, memory_order_release);
                break;
        }
        pthread_mutex_unlock(&g_registry_lock);
}

static void rx_dispatch_fixed(struct rx_dispatch_result *out,
                              bool parse_fallback) {
        out->action = RX_DISPATCH_DELIVER;
        out->worker_index = 0;
        out->owner_hit = false;
        out->software_hash = false;
        out->parse_fallback = parse_fallback;
}

static bool rx_dispatch_apply_owner(int worker,
                                    struct rx_dispatch_result *out) {
        if (worker < 0)
                return false;
        out->action = RX_DISPATCH_DELIVER;
        out->worker_index = (uint16_t)worker;
        out->owner_hit = true;
        out->software_hash = false;
        out->parse_fallback = false;
        return true;
}

static void rx_dispatch_hash_or_hardware(uint8_t protocol, uint32_t remote_ip,
                                         uint32_t local_ip,
                                         uint16_t remote_port,
                                         uint16_t local_port,
                                         uint16_t rx_queue,
                                         struct rx_dispatch_result *out) {
        int worker;

        if (port_rx_uses_hardware_rss(protocol) && rx_queue < g_worker_count) {
                out->action = RX_DISPATCH_DELIVER;
                out->worker_index = rx_queue;
                out->owner_hit = false;
                out->software_hash = false;
                out->parse_fallback = false;
                return;
        }
        worker = port_flow_queue_for_ipv4(protocol, remote_ip, local_ip,
                                          remote_port, local_port);
        if (worker >= 0 && (uint16_t)worker < g_worker_count) {
                out->action = RX_DISPATCH_DELIVER;
                out->worker_index = (uint16_t)worker;
                out->owner_hit = false;
                out->software_hash = true;
                out->parse_fallback = false;
                return;
        }
        rx_dispatch_fixed(out, false);
}

void rx_dispatch_classify(const struct rte_mbuf *mbuf, uint16_t rx_queue,
                          struct rx_dispatch_result *out) {
        const struct rte_ether_hdr *eth;
        const struct rte_ipv4_hdr *ip;
        const uint8_t *l4;
        uint16_t l4_available;

        if (out == NULL)
                return;
        rx_dispatch_fixed(out, false);
        if (mbuf == NULL || g_worker_count == 0)
                return;
        if (mbuf->data_len < sizeof(*eth) || mbuf->pkt_len < sizeof(*eth)) {
                out->parse_fallback = true;
                return;
        }

        eth = rte_pktmbuf_mtod(mbuf, const struct rte_ether_hdr *);
        if (eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
                out->action = RX_DISPATCH_FANOUT;
                return;
        }
        if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                out->parse_fallback = true;
                return;
        }
        if (mbuf->data_len < sizeof(*eth) + sizeof(*ip) ||
            mbuf->pkt_len < sizeof(*eth) + sizeof(*ip)) {
                out->parse_fallback = true;
                return;
        }

        ip = rte_pktmbuf_mtod_offset(mbuf, const struct rte_ipv4_hdr *,
                                     sizeof(*eth));
        if ((ip->version_ihl >> 4) != 4 ||
            rte_ipv4_hdr_len(ip) != sizeof(*ip) ||
            (rte_be_to_cpu_16(ip->fragment_offset) &
             (RTE_IPV4_HDR_OFFSET_MASK | RTE_IPV4_HDR_MF_FLAG)) != 0) {
                out->parse_fallback = true;
                return;
        }
        l4 = (const uint8_t *)(ip + 1);
        l4_available = mbuf->data_len - sizeof(*eth) - sizeof(*ip);

        if (ip->next_proto_id == IPPROTO_TCP) {
                const struct rte_tcp_hdr *tcp;
                int owner;

                if (l4_available < sizeof(*tcp)) {
                        out->parse_fallback = true;
                        return;
                }
                tcp = (const struct rte_tcp_hdr *)l4;
                owner = rx_dispatch_lookup_tcp_connection(
                    ip->src_addr, ip->dst_addr, tcp->src_port, tcp->dst_port);
                if (rx_dispatch_apply_owner(owner, out))
                        return;
                owner = rx_dispatch_lookup_endpoint(IPPROTO_TCP, ip->dst_addr,
                                                    tcp->dst_port);
                if (owner < 0 && ip->dst_addr != INADDR_ANY)
                        owner = rx_dispatch_lookup_endpoint(
                            IPPROTO_TCP, INADDR_ANY, tcp->dst_port);
                if (rx_dispatch_apply_owner(owner, out))
                        return;
                rx_dispatch_hash_or_hardware(
                    IPPROTO_TCP, ip->src_addr, ip->dst_addr, tcp->src_port,
                    tcp->dst_port, rx_queue, out);
                return;
        }
        if (ip->next_proto_id == IPPROTO_UDP) {
                const struct rte_udp_hdr *udp;
                int owner;

                if (l4_available < sizeof(*udp)) {
                        out->parse_fallback = true;
                        return;
                }
                udp = (const struct rte_udp_hdr *)l4;
                owner = rx_dispatch_lookup_endpoint(IPPROTO_UDP, ip->dst_addr,
                                                    udp->dst_port);
                if (owner < 0 && ip->dst_addr != INADDR_ANY)
                        owner = rx_dispatch_lookup_endpoint(
                            IPPROTO_UDP, INADDR_ANY, udp->dst_port);
                if (rx_dispatch_apply_owner(owner, out))
                        return;
                rx_dispatch_hash_or_hardware(
                    IPPROTO_UDP, ip->src_addr, ip->dst_addr, udp->src_port,
                    udp->dst_port, rx_queue, out);
                return;
        }
        if (ip->next_proto_id == IPPROTO_ICMP && l4_available >= 6) {
                uint16_t identifier;
                uint16_t type_code = ((uint16_t)l4[0] << 8) | l4[1];

                memcpy(&identifier, l4 + 4, sizeof(identifier));
                rx_dispatch_hash_or_hardware(
                    IPPROTO_ICMP, ip->src_addr, ip->dst_addr, identifier,
                    type_code, rx_queue, out);
                return;
        }
        rx_dispatch_hash_or_hardware(ip->next_proto_id, ip->src_addr,
                                     ip->dst_addr, 0, 0, rx_queue, out);
}
