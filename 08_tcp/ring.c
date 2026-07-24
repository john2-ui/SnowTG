#include "ring.h"

#include "config.h"
#include "log.h"

#include <rte_eal.h>
#include <rte_malloc.h>

static struct inout_ring *r_instance = NULL;

struct inout_ring *ring_instance(void) {
        if (r_instance != NULL)
                return r_instance;

        r_instance = rte_malloc("inout_ring", sizeof(struct inout_ring), 0);
        if (r_instance == NULL)
                rte_exit(EXIT_FAILURE, "rte_malloc(inout_ring) failed\n");

        memset(r_instance, 0, sizeof(struct inout_ring));
        r_instance->in =
            rte_ring_create("in_ring", RING_SIZE, rte_socket_id(), 0);
        r_instance->out =
            rte_ring_create("out_ring", RING_SIZE, rte_socket_id(), 0);

        if (r_instance->in == NULL || r_instance->out == NULL)
                rte_exit(EXIT_FAILURE, "rte_ring_create() failed\n");

        LOG_INFO("rx/tx rings created (size=%d)", RING_SIZE);
        return r_instance;
}
