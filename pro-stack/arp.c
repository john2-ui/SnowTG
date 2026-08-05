#include "arp.h"

#include "list.h"
#include "log.h"
#include "net_context.h"

#include <rte_arp.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <string.h>

static struct arp_table *arpt = NULL;

struct arp_table *arp_table_instance(void) {
        if (arpt != NULL)
                return arpt;

        arpt = rte_malloc("arp_table", sizeof(struct arp_table), 0);
        if (arpt == NULL)
                rte_exit(EXIT_FAILURE, "rte_malloc(arp_table) failed\n");

        memset(arpt, 0, sizeof(struct arp_table));
        return arpt;
}

uint8_t *arp_lookup(uint32_t ip) {
        struct arp_table *table = arp_table_instance();
        struct arp_entry *iter;

        for (iter = table->entries; iter != NULL; iter = iter->next) {
                if (iter->ip == ip)
                        return iter->hwaddr;
        }
        return NULL;
}

void arp_table_add(uint32_t ip, const uint8_t *mac) {
        struct arp_table *table = arp_table_instance();

        if (arp_lookup(ip) != NULL)
                return;

        struct arp_entry *entry =
            rte_malloc("arp_entry", sizeof(struct arp_entry), 0);
        if (entry == NULL) {
                LOG_ERROR("rte_malloc(arp_entry) failed, dropping mapping");
                return;
        }

        memset(entry, 0, sizeof(struct arp_entry));
        entry->ip = ip;
        rte_memcpy(entry->hwaddr, mac, RTE_ETHER_ADDR_LEN);
        entry->type = 0;

        LL_ADD(entry, table->entries);
        table->count++;
        /* TODO: ARP cache aging/eviction. Entries are never expired or
         * replaced, so the table grows for the life of the process and stale
         * MAC bindings persist forever. Add a last-seen timestamp and a
         * periodic sweep (or an LRU cap). */

        LOG_INFO("arp learn " IP_FMT " -> " MAC_FMT " (entries=%d)", IP_ARG(ip),
                 MAC_ARG(mac), table->count);
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
        struct rte_arp_hdr *arp = rte_pktmbuf_mtod_offset(
            mbuf, struct rte_arp_hdr *, sizeof(struct rte_ether_hdr));

        /* Only packets addressed to us are interesting. */
        if (arp->arp_data.arp_tip != g_net.local_ip) {
                rte_pktmbuf_free(mbuf);
                return;
        }

        uint16_t opcode = rte_be_to_cpu_16(arp->arp_opcode);

        if (opcode == RTE_ARP_OP_REQUEST) {
                arp_table_add(arp->arp_data.arp_sip,
                              arp->arp_data.arp_sha.addr_bytes);
                LOG_INFO("arp request from " IP_FMT ", sending reply",
                         IP_ARG(arp->arp_data.arp_sip));

                struct rte_mbuf *reply = arp_build_pkt(
                    mp, RTE_ARP_OP_REPLY, arp->arp_data.arp_sha.addr_bytes,
                    arp->arp_data.arp_tip, arp->arp_data.arp_sip);
                if (reply != NULL)
                        rte_ring_mp_enqueue_burst(out, (void **)&reply, 1,
                                                  NULL);
        } else if (opcode == RTE_ARP_OP_REPLY) {
                LOG_DEBUG("arp reply from " IP_FMT,
                          IP_ARG(arp->arp_data.arp_sip));
                arp_table_add(arp->arp_data.arp_sip,
                              arp->arp_data.arp_sha.addr_bytes);
        }

        rte_pktmbuf_free(mbuf);
}
