#include "arp.h"

#include "config.h"
#include "log.h"
#include "net_context.h"

#include <rte_arp.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <string.h>

static struct arp_table *arpt = NULL;

static uint64_t arp_elapsed_ms(uint64_t now, uint64_t then) {
        if (now < then)
                return UINT64_MAX;

        return (now - then) * 1000U / rte_get_timer_hz();
}

static void arp_remove(struct arp_table *table, struct arp_entry *entry) {
        if (entry->state == ARP_STATE_FREE)
                return;

        rte_hash_del_key(table->hash, &entry->ip);
        memset(entry, 0, sizeof(*entry));
        table->count--;
}

static struct arp_entry *arp_find(struct arp_table *table, uint32_t ip) {
        struct arp_entry *entry = NULL;

        if (rte_hash_lookup_data(table->hash, &ip, (void **)&entry) < 0)
                return NULL;
        return entry;
}

static struct arp_entry *arp_allocate(struct arp_table *table, uint64_t now) {
        struct arp_entry *lru = NULL;

        for (uint32_t i = 0; i < table->capacity; i++) {
                struct arp_entry *entry = &table->entries[i];

                if (entry->state == ARP_STATE_FREE)
                        return entry;
                if (entry->state == ARP_STATE_REACHABLE &&
                    arp_elapsed_ms(now, entry->confirmed_at) >=
                        ARP_REACHABLE_TTL_MS) {
                        arp_remove(table, entry);
                        return entry;
                }
                if (entry->state == ARP_STATE_FAILED &&
                    arp_elapsed_ms(now, entry->last_probe_at) >=
                        ARP_FAILED_TTL_MS) {
                        arp_remove(table, entry);
                        return entry;
                }
                if (entry->state == ARP_STATE_REACHABLE &&
                    (lru == NULL || entry->last_used_at < lru->last_used_at))
                        lru = entry;
        }

        if (lru != NULL) {
                arp_remove(table, lru);
                return lru;
        }
        return NULL;
}

static struct arp_entry *arp_create(struct arp_table *table, uint32_t ip,
                                    uint64_t now) {
        struct arp_entry *entry = arp_allocate(table, now);

        if (entry == NULL) {
                LOG_WARN("ARP cache full; all entries are resolving");
                return NULL;
        }

        memset(entry, 0, sizeof(*entry));
        entry->ip = ip;
        entry->state = ARP_STATE_INCOMPLETE;
        entry->last_used_at = now;
        if (rte_hash_add_key_data(table->hash, &entry->ip, entry) < 0) {
                LOG_ERROR("failed to insert ARP cache entry for " IP_FMT,
                          IP_ARG(ip));
                memset(entry, 0, sizeof(*entry));
                return NULL;
        }
        table->count++;
        return entry;
}

static int arp_send_request(struct rte_mempool *mp, struct rte_ring *out,
                            uint32_t ip) {
        struct rte_mbuf *request = arp_build_pkt(
            mp, RTE_ARP_OP_REQUEST, g_broadcast_mac, g_net.local_ip, ip);

        if (request == NULL)
                return -1;
        if (rte_ring_mp_enqueue_burst(out, (void **)&request, 1, NULL) != 1) {
                LOG_WARN("ARP request dropped because TX ring is full");
                rte_pktmbuf_free(request);
                return -1;
        }
        return 0;
}

static void arp_probe(struct arp_entry *entry, struct rte_mempool *mp,
                      struct rte_ring *out, uint64_t now) {
        entry->last_probe_at = now;
        if (arp_send_request(mp, out, entry->ip) == 0)
                entry->probe_count++;
}

struct arp_table *arp_table_instance(void) {
        if (arpt != NULL)
                return arpt;

        arpt = rte_zmalloc("arp_table", sizeof(*arpt), 0);
        if (arpt == NULL)
                rte_exit(EXIT_FAILURE, "rte_malloc(arp_table) failed\n");

        arpt->capacity = ARP_CACHE_CAPACITY;
        arpt->entries =
            rte_zmalloc("arp_entries", sizeof(*arpt->entries) * arpt->capacity,
                        RTE_CACHE_LINE_SIZE);
        if (arpt->entries == NULL)
                rte_exit(EXIT_FAILURE, "rte_malloc(arp_entries) failed\n");

        struct rte_hash_parameters params = {
            .name = "arp_cache",
            .entries = ARP_CACHE_CAPACITY,
            .key_len = sizeof(uint32_t),
            .hash_func = rte_jhash,
            .hash_func_init_val = 0,
            .socket_id = rte_socket_id(),
        };
        arpt->hash = rte_hash_create(&params);
        if (arpt->hash == NULL)
                rte_exit(EXIT_FAILURE,
                         "rte_hash_create(arp_cache) failed: %s\n",
                         rte_strerror(rte_errno));
        return arpt;
}

const uint8_t *arp_resolve(struct rte_mempool *mp, struct rte_ring *out,
                           uint32_t ip, uint64_t now) {
        struct arp_table *table = arp_table_instance();
        struct arp_entry *entry = arp_find(table, ip);

        if (entry == NULL) {
                entry = arp_create(table, ip, now);
                if (entry == NULL)
                        return NULL;
                arp_probe(entry, mp, out, now);
                return NULL;
        }

        switch (entry->state) {
        case ARP_STATE_REACHABLE:
                if (arp_elapsed_ms(now, entry->confirmed_at) <
                    ARP_REACHABLE_TTL_MS) {
                        entry->last_used_at = now;
                        return entry->hwaddr;
                }
                arp_remove(table, entry);
                entry = arp_create(table, ip, now);
                if (entry != NULL)
                        arp_probe(entry, mp, out, now);
                return NULL;
        case ARP_STATE_INCOMPLETE:
                if (arp_elapsed_ms(now, entry->last_probe_at) >=
                    ARP_PROBE_INTERVAL_MS) {
                        if (entry->probe_count < ARP_PROBE_MAX_RETRIES)
                                arp_probe(entry, mp, out, now);
                        else {
                                entry->state = ARP_STATE_FAILED;
                                entry->last_probe_at = now;
                        }
                }
                return NULL;
        case ARP_STATE_FAILED:
                if (arp_elapsed_ms(now, entry->last_probe_at) >=
                    ARP_FAILED_TTL_MS) {
                        entry->state = ARP_STATE_INCOMPLETE;
                        entry->probe_count = 0;
                        arp_probe(entry, mp, out, now);
                }
                return NULL;
        case ARP_STATE_FREE:
        default:
                return NULL;
        }
}

static void arp_table_learn_at(struct arp_table *table, uint32_t ip,
                               const uint8_t *mac, uint64_t now) {
        struct arp_entry *entry = arp_find(table, ip);

        if (entry == NULL) {
                entry = arp_create(table, ip, now);
                if (entry == NULL)
                        return;
        } else if (entry->state == ARP_STATE_REACHABLE &&
                   memcmp(entry->hwaddr, mac, RTE_ETHER_ADDR_LEN) == 0) {
                entry->confirmed_at = now;
                entry->last_used_at = now;
                return;
        } else if (entry->state == ARP_STATE_REACHABLE &&
                   memcmp(entry->hwaddr, mac, RTE_ETHER_ADDR_LEN) != 0) {
                LOG_WARN("ARP MAC changed for " IP_FMT " from " MAC_FMT
                         " to " MAC_FMT,
                         IP_ARG(ip), MAC_ARG(entry->hwaddr), MAC_ARG(mac));
        }

        rte_memcpy(entry->hwaddr, mac, RTE_ETHER_ADDR_LEN);
        entry->state = ARP_STATE_REACHABLE;
        entry->probe_count = 0;
        entry->confirmed_at = now;
        entry->last_used_at = now;
        LOG_ARP_INFO("arp learn " IP_FMT " -> " MAC_FMT " (entries=%u)",
                     IP_ARG(ip), MAC_ARG(mac), table->count);
}

void arp_table_learn(uint32_t ip, const uint8_t *mac) {
        struct arp_table *table = arp_table_instance();

        arp_table_learn_at(table, ip, mac, rte_get_timer_cycles());
}

void arp_table_confirm(uint32_t ip, const uint8_t *mac) {
        struct arp_table *table = arp_table_instance();
        uint64_t now = rte_get_timer_cycles();
        struct arp_entry *entry = arp_find(table, ip);

        if (entry != NULL && entry->state == ARP_STATE_REACHABLE &&
            memcmp(entry->hwaddr, mac, RTE_ETHER_ADDR_LEN) == 0) {
                entry->confirmed_at = now;
                entry->last_used_at = now;
                return;
        }

        arp_table_learn_at(table, ip, mac, now);
}

void arp_maintain(uint64_t now) {
        struct arp_table *table = arp_table_instance();

        for (uint32_t i = 0; i < table->capacity; i++) {
                struct arp_entry *entry = &table->entries[i];

                if (entry->state == ARP_STATE_REACHABLE &&
                    arp_elapsed_ms(now, entry->confirmed_at) >=
                        ARP_REACHABLE_TTL_MS) {
                        arp_remove(table, entry);
                } else if (entry->state == ARP_STATE_FAILED &&
                           arp_elapsed_ms(now, entry->last_probe_at) >=
                               ARP_FAILED_TTL_MS) {
                        arp_remove(table, entry);
                }
        }
}

void arp_debug_sweep(struct rte_mempool *mp, struct rte_ring *out,
                     uint64_t now) {
        static uint8_t next_host = 1;

        for (uint32_t i = 0; i < ARP_SWEEP_BATCH; i++) {
                uint32_t ip = (g_net.local_ip & 0x00ffffffU) |
                              ((uint32_t)next_host << 24);
                (void)arp_resolve(mp, out, ip, now);
                next_host = (next_host == 254U) ? 1U : next_host + 1U;
        }
}

static int arp_frame_is_valid(const struct rte_mbuf *mbuf,
                              const struct rte_arp_hdr *arp) {
        if (mbuf->pkt_len < sizeof(struct rte_ether_hdr) + sizeof(*arp) ||
            mbuf->data_len < sizeof(struct rte_ether_hdr) + sizeof(*arp) ||
            arp->arp_hardware != rte_cpu_to_be_16(RTE_ARP_HRD_ETHER) ||
            arp->arp_protocol != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) ||
            arp->arp_hlen != RTE_ETHER_ADDR_LEN ||
            arp->arp_plen != sizeof(uint32_t))
                return 0;

        const uint8_t *mac = arp->arp_data.arp_sha.addr_bytes;
        for (unsigned int i = 0; i < RTE_ETHER_ADDR_LEN; i++) {
                if (mac[i] != 0)
                        return (mac[0] & 1U) == 0;
        }
        return 0;
}

struct rte_mbuf *arp_build_pkt(struct rte_mempool *mp, uint16_t opcode,
                               const uint8_t *dst_mac, uint32_t src_ip,
                               uint32_t dst_ip) {
        const unsigned int total_len =
            sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);

        struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mp);
        if (mbuf == NULL) {
                unsigned int available =
                    mp == NULL ? 0 : rte_mempool_avail_count(mp);
                unsigned int in_use =
                    mp == NULL ? 0 : rte_mempool_in_use_count(mp);

                LOG_ERROR("rte_pktmbuf_alloc() failed mp=%p avail=%u "
                          "in_use=%u rte_errno=%d (%s)",
                          (void *)mp, available, in_use, rte_errno,
                          rte_strerror(rte_errno));
                return NULL;
        }

        mbuf->pkt_len = total_len;
        mbuf->data_len = total_len;

        uint8_t *msg = rte_pktmbuf_mtod(mbuf, uint8_t *);

        /* Ethernet header */
        struct rte_ether_hdr *eth = (struct rte_ether_hdr *)msg;
        rte_memcpy(eth->src_addr.addr_bytes, g_net.local_mac,
                   RTE_ETHER_ADDR_LEN);
        rte_memcpy(eth->dst_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

        /* ARP header */
        struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);
        arp->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
        arp->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
        arp->arp_hlen = RTE_ETHER_ADDR_LEN;
        arp->arp_plen = sizeof(uint32_t);
        arp->arp_opcode = rte_cpu_to_be_16(opcode);

        rte_memcpy(arp->arp_data.arp_sha.addr_bytes, g_net.local_mac,
                   RTE_ETHER_ADDR_LEN);
        rte_memcpy(arp->arp_data.arp_tha.addr_bytes, dst_mac,
                   RTE_ETHER_ADDR_LEN);
        arp->arp_data.arp_sip = src_ip;
        arp->arp_data.arp_tip = dst_ip;

        return mbuf;
}

void arp_handle(struct rte_mempool *mp, struct rte_mbuf *mbuf,
                struct rte_ring *out) {
        if (mbuf->pkt_len <
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr) ||
            mbuf->data_len <
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr)) {
                rte_pktmbuf_free(mbuf);
                return;
        }
        struct rte_arp_hdr *arp = rte_pktmbuf_mtod_offset(
            mbuf, struct rte_arp_hdr *, sizeof(struct rte_ether_hdr));

        if (!arp_frame_is_valid(mbuf, arp)) {
                rte_pktmbuf_free(mbuf);
                return;
        }

        /* Only packets addressed to us are interesting. */
        if (arp->arp_data.arp_tip != g_net.local_ip) {
                rte_pktmbuf_free(mbuf);
                return;
        }

        uint16_t opcode = rte_be_to_cpu_16(arp->arp_opcode);
        if (opcode != RTE_ARP_OP_REQUEST && opcode != RTE_ARP_OP_REPLY) {
                rte_pktmbuf_free(mbuf);
                return;
        }

        if (opcode == RTE_ARP_OP_REQUEST) {
                arp_table_learn(arp->arp_data.arp_sip,
                                arp->arp_data.arp_sha.addr_bytes);
                LOG_ARP_INFO("arp request from " IP_FMT ", sending reply",
                             IP_ARG(arp->arp_data.arp_sip));

                struct rte_mbuf *reply = arp_build_pkt(
                    mp, RTE_ARP_OP_REPLY, arp->arp_data.arp_sha.addr_bytes,
                    arp->arp_data.arp_tip, arp->arp_data.arp_sip);
                if (reply != NULL && rte_ring_mp_enqueue_burst(
                                         out, (void **)&reply, 1, NULL) != 1)
                        rte_pktmbuf_free(reply);
        } else if (opcode == RTE_ARP_OP_REPLY) {
                LOG_ARP_DEBUG("arp reply from " IP_FMT,
                              IP_ARG(arp->arp_data.arp_sip));
                arp_table_learn(arp->arp_data.arp_sip,
                                arp->arp_data.arp_sha.addr_bytes);
        }

        rte_pktmbuf_free(mbuf);
}
