#include "ipv4_reassembly.h"

#include "config.h"
#include "log.h"

#include <errno.h>
#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <stdbool.h>
#include <string.h>

_Static_assert(RTE_LIBRTE_IP_FRAG_MAX_FRAG == UDP_SENDTO_MAX_DATAGRAMS,
               "this stack requires DPDK's IPv4 reassembly limit to be 8");

static struct rte_mbuf *ipv4_reassembly_drop(struct rte_mbuf *mbuf) {
        if (mbuf != NULL)
                rte_pktmbuf_free(mbuf);
        return NULL;
}

int ipv4_reassembly_init(struct ipv4_reassembly *ctx) {
        const uint32_t bucket_entries = 4;
        const uint32_t bucket_num = IPV4_REASSEMBLY_MAX_ENTRIES;
        uint64_t hz;

        if (ctx == NULL) {
                errno = EINVAL;
                return -1;
        }
        memset(ctx, 0, sizeof(*ctx));
        hz = rte_get_timer_hz();
        ctx->table = rte_ip_frag_table_create(
            bucket_num, bucket_entries, IPV4_REASSEMBLY_MAX_ENTRIES,
            hz * IPV4_REASSEMBLY_TIMEOUT_MS / 1000U, rte_socket_id());
        if (ctx->table == NULL) {
                errno = ENOMEM;
                return -1;
        }
        ctx->sweep_interval_cycles = hz * IPV4_REASSEMBLY_SWEEP_MS / 1000U;
        ctx->last_sweep_cycles = rte_get_timer_cycles();
        LOG_INFO("IPv4 reassembly initialized entries=%u fragments=%u "
                 "timeout_ms=%u",
                 IPV4_REASSEMBLY_MAX_ENTRIES,
                 (unsigned int)RTE_LIBRTE_IP_FRAG_MAX_FRAG,
                 IPV4_REASSEMBLY_TIMEOUT_MS);
        return 0;
}

struct rte_mbuf *ipv4_reassembly_process(struct ipv4_reassembly *ctx,
                                         struct rte_mbuf *mbuf,
                                         uint64_t now_cycles) {
        const uint16_t eth_len = sizeof(struct rte_ether_hdr);
        const uint16_t ip_len = sizeof(struct rte_ipv4_hdr);
        const uint16_t reserved_flag = UINT16_C(1) << 15;
        struct rte_ether_hdr *eth;
        struct rte_ipv4_hdr *ip;
        uint16_t total_len;
        uint16_t frag;
        uint16_t payload_len;
        uint32_t offset;
        bool fragmented;

        if (ctx == NULL || ctx->table == NULL || mbuf == NULL)
                return ipv4_reassembly_drop(mbuf);
        if (mbuf->data_len < eth_len || mbuf->pkt_len < eth_len)
                return ipv4_reassembly_drop(mbuf);
        eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
                return mbuf;
        if (mbuf->data_len < eth_len + ip_len ||
            mbuf->pkt_len < eth_len + ip_len)
                return ipv4_reassembly_drop(mbuf);
        ip = rte_pktmbuf_mtod_offset(mbuf, struct rte_ipv4_hdr *, eth_len);
        frag = rte_be_to_cpu_16(ip->fragment_offset);
        fragmented =
            (frag & (RTE_IPV4_HDR_OFFSET_MASK | RTE_IPV4_HDR_MF_FLAG |
                     reserved_flag)) != 0;
        if (!fragmented)
                return mbuf;

        if ((ip->version_ihl >> 4) != 4 || rte_ipv4_hdr_len(ip) != ip_len ||
            rte_ipv4_cksum(ip) != 0)
                return ipv4_reassembly_drop(mbuf);
        total_len = rte_be_to_cpu_16(ip->total_length);
        if (total_len <= ip_len || total_len > mbuf->pkt_len - eth_len ||
            (frag & (RTE_IPV4_HDR_DF_FLAG | reserved_flag)) != 0)
                return ipv4_reassembly_drop(mbuf);
        payload_len = total_len - ip_len;
        if ((frag & RTE_IPV4_HDR_MF_FLAG) != 0 &&
            (payload_len % RTE_IPV4_HDR_OFFSET_UNITS) != 0)
                return ipv4_reassembly_drop(mbuf);
        offset = (uint32_t)(frag & RTE_IPV4_HDR_OFFSET_MASK) *
                 RTE_IPV4_HDR_OFFSET_UNITS;
        if (offset + payload_len + ip_len > UINT16_MAX)
                return ipv4_reassembly_drop(mbuf);

        mbuf->l2_len = eth_len;
        mbuf->l3_len = ip_len;
        struct rte_mbuf *result = rte_ipv4_frag_reassemble_packet(
            ctx->table, &ctx->death_row, mbuf, now_cycles, ip);
        rte_ip_frag_free_death_row(&ctx->death_row, 3);
        if (result == NULL)
                return NULL;

        ip = rte_pktmbuf_mtod_offset(result, struct rte_ipv4_hdr *,
                                     result->l2_len);
        ip->fragment_offset &= rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
        ip->hdr_checksum = 0;
        ip->hdr_checksum = rte_ipv4_cksum(ip);
        return result;
}

void ipv4_reassembly_maintain(struct ipv4_reassembly *ctx,
                              uint64_t now_cycles) {
        if (ctx == NULL || ctx->table == NULL ||
            now_cycles - ctx->last_sweep_cycles < ctx->sweep_interval_cycles)
                return;
        rte_ip_frag_table_del_expired_entries(ctx->table, &ctx->death_row,
                                              now_cycles);
        rte_ip_frag_free_death_row(&ctx->death_row, 3);
        ctx->last_sweep_cycles = now_cycles;
}

void ipv4_reassembly_fini(struct ipv4_reassembly *ctx) {
        if (ctx == NULL)
                return;
        if (ctx->table != NULL) {
                rte_ip_frag_table_del_expired_entries(
                    ctx->table, &ctx->death_row, UINT64_MAX);
                rte_ip_frag_free_death_row(&ctx->death_row, 0);
                rte_ip_frag_table_destroy(ctx->table);
        }
        memset(ctx, 0, sizeof(*ctx));
}
