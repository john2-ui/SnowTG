#ifndef TRAFFIC_GEN_FLOW_POOL_H
#define TRAFFIC_GEN_FLOW_POOL_H

/**
 * @file flow_pool.h
 * @brief Owner-local fixed-capacity pool for traffic-generator flows.
 */

#include "flow.h"

#include <stdlib.h>

struct tg_flow_pool {
        struct tg_flow *flows;
        uint32_t *free_ids;
        uint32_t capacity;
        uint32_t free_count;
};

int tg_flow_pool_init(struct tg_flow_pool *pool, uint32_t capacity);
void tg_flow_pool_fini(struct tg_flow_pool *pool);
struct tg_flow *tg_flow_pool_get(struct tg_flow_pool *pool);
int tg_flow_pool_put(struct tg_flow_pool *pool, struct tg_flow *flow);

#endif /* TRAFFIC_GEN_FLOW_POOL_H */
