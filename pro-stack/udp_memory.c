#include "udp_memory.h"

#include "config.h"
#include "log.h"

#include <errno.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <stdio.h>
#include <string.h>

static struct udp_rx_node *udp_memory_get(struct udp_owner_memory *memory) {
        struct udp_rx_node *node;

        if (memory == NULL || memory->rx_nodes == NULL ||
            rte_mempool_get(memory->rx_nodes, (void **)&node) != 0) {
                if (memory != NULL)
                        memory->alloc_fail++;
                errno = ENOBUFS;
                return NULL;
        }

        memset(node, 0, sizeof(*node));
        uint32_t available = rte_mempool_avail_count(memory->rx_nodes);
        uint32_t used = memory->capacity - available;
        if (used > memory->peak_in_use)
                memory->peak_in_use = used;
        return node;
}

/** @copydoc udp_owner_memory_init */
int udp_owner_memory_init(struct udp_owner_memory *memory,
                          unsigned int lcore_id) {
        char name[RTE_MEMPOOL_NAMESIZE];

        if (memory == NULL)
                return -1;
        memset(memory, 0, sizeof(*memory));
        memory->lcore_id = (uint16_t)lcore_id;
        (void)snprintf(name, sizeof(name), "udp_rx_node_%u", lcore_id);
        memory->rx_nodes = rte_mempool_create(
            name, UDP_MEMORY_RX_NODES, sizeof(struct udp_rx_node), 0, 0, NULL,
            NULL, NULL, NULL, rte_socket_id(), 0);
        if (memory->rx_nodes == NULL) {
                LOG_ERROR("UDP memory pool init failed count=%u size=%zu "
                          "errno=%d (%s)",
                          UDP_MEMORY_RX_NODES, sizeof(struct udp_rx_node),
                          rte_errno, rte_strerror(rte_errno));
                memset(memory, 0, sizeof(*memory));
                return -1;
        }
        memory->capacity = UDP_MEMORY_RX_NODES;
        return 0;
}

/** @copydoc udp_owner_memory_fini */
void udp_owner_memory_fini(struct udp_owner_memory *memory) {
        if (memory == NULL)
                return;
        if (memory->rx_nodes != NULL)
                rte_mempool_free(memory->rx_nodes);
        memset(memory, 0, sizeof(*memory));
}

/** @copydoc udp_owner_memory_snapshot */
void udp_owner_memory_snapshot(const struct udp_owner_memory *memory,
                               struct udp_memory_snapshot *snapshot) {
        if (snapshot == NULL)
                return;
        memset(snapshot, 0, sizeof(*snapshot));
        if (memory == NULL)
                return;
        snapshot->capacity = memory->capacity;
        snapshot->available = memory->rx_nodes == NULL
                                  ? 0
                                  : rte_mempool_avail_count(memory->rx_nodes);
        snapshot->alloc_fail = memory->alloc_fail;
        snapshot->peak_in_use = memory->peak_in_use;
        snapshot->queue_drops = memory->queue_drops;
}

/** @copydoc udp_memory_rx_node_alloc */
struct udp_rx_node *udp_memory_rx_node_alloc(struct udp_owner_memory *memory) {
        return udp_memory_get(memory);
}

/** @copydoc udp_memory_rx_node_free */
void udp_memory_rx_node_free(struct udp_owner_memory *memory,
                             struct udp_rx_node *node) {
        if (memory != NULL && memory->rx_nodes != NULL && node != NULL)
                rte_mempool_put(memory->rx_nodes, node);
}

/** @copydoc udp_memory_record_queue_drop */
void udp_memory_record_queue_drop(struct udp_owner_memory *memory) {
        if (memory != NULL)
                memory->queue_drops++;
}
