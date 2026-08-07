#include "ring.h"

#include "config.h"
#include "log.h"

#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_malloc.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static struct inout_ring r_instances[RTE_MAX_LCORE];
static bool r_ready[RTE_MAX_LCORE];

int ring_init_owner(unsigned int lcore_id) {
        struct inout_ring *ring;
        char in_name[RTE_RING_NAMESIZE];
        char out_name[RTE_RING_NAMESIZE];

        if (lcore_id >= RTE_MAX_LCORE)
                return -1;
        if (r_ready[lcore_id])
                return 0;

        ring = &r_instances[lcore_id];
        memset(ring, 0, sizeof(*ring));
        (void)snprintf(in_name, sizeof(in_name), "in_ring_%u", lcore_id);
        (void)snprintf(out_name, sizeof(out_name), "out_ring_%u", lcore_id);
        ring->in = rte_ring_create(in_name, RING_SIZE, rte_socket_id(),
                                   RING_F_SP_ENQ | RING_F_SC_DEQ);
        ring->out = rte_ring_create(out_name, RING_SIZE, rte_socket_id(),
                                    RING_F_SP_ENQ | RING_F_SC_DEQ);

        if (ring->in == NULL || ring->out == NULL) {
                if (ring->in != NULL)
                        rte_ring_free(ring->in);
                if (ring->out != NULL)
                        rte_ring_free(ring->out);
                memset(ring, 0, sizeof(*ring));
                return -1;
        }

        r_ready[lcore_id] = true;
        LOG_INFO("rx/tx rings created lcore=%u size=%d", lcore_id, RING_SIZE);
        return 0;
}

struct inout_ring *ring_for_lcore(unsigned int lcore_id) {
        if (lcore_id >= RTE_MAX_LCORE || !r_ready[lcore_id])
                return NULL;
        return &r_instances[lcore_id];
}

struct inout_ring *ring_instance(void) {
        return ring_for_lcore(rte_lcore_id());
}

void ring_fini(void) {
        for (unsigned int lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
                if (!r_ready[lcore_id])
                        continue;
                rte_ring_free(r_instances[lcore_id].in);
                rte_ring_free(r_instances[lcore_id].out);
                memset(&r_instances[lcore_id], 0, sizeof(r_instances[lcore_id]));
                r_ready[lcore_id] = false;
        }
}
