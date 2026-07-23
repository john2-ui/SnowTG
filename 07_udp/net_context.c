#include "net_context.h"

#include "log.h"

#include <rte_ethdev.h>

struct net_context g_net;

void net_context_init(uint16_t port_id, uint32_t local_ip) {
        g_net.port_id = port_id;
        g_net.local_ip = local_ip;
        rte_eth_macaddr_get(port_id, (struct rte_ether_addr *)g_net.local_mac);

        LOG_INFO("local identity: port=%u ip=" IP_FMT " mac=" MAC_FMT, port_id,
                 IP_ARG(local_ip), MAC_ARG(g_net.local_mac));
}

void net_context_set_mempool(struct rte_mempool *mp) { g_net.mp = mp; }
