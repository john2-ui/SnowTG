#ifndef TRAFFIC_GEN_CONN_POOL_H
#define TRAFFIC_GEN_CONN_POOL_H

/**
 * @file conn_pool.h
 * @brief Owner-local fixed-capacity pool for traffic-generator flows.
 */

#include "flow.h"

#include <stdlib.h>

/**
 * Preallocated owner-local flow pool.
 *
 * The free stack stores indexes into @ref flows.  All access occurs on one
 * owner lcore, so allocation and return require neither locks nor atomics.
 */
struct tg_conn_pool {
        struct tg_flow *flows;
        uint32_t *free_ids;
        uint32_t capacity;
        uint32_t free_count;
};

/**
 * Initialize a fixed-capacity owner-local flow pool.
 * @return 0 on success, or -1 with errno set on invalid input or allocation
 *         failure.
 */
int tg_conn_pool_init(struct tg_conn_pool *pool, uint32_t capacity);

/** Release pool storage after every checked-out flow has been returned. */
void tg_conn_pool_fini(struct tg_conn_pool *pool);

/**
 * Get one reset flow object from the pool.
 * @return A flow object, or NULL with errno set to ENOBUFS when exhausted.
 */
struct tg_flow *tg_conn_pool_get(struct tg_conn_pool *pool);

/**
 * Return an unmapped flow to the pool.
 * @return 0 on success, or -1 with errno set when @p flow is invalid, already
 *         returned, or still registered in a flow map.
 */
int tg_conn_pool_put(struct tg_conn_pool *pool, struct tg_flow *flow);
#endif /* TRAFFIC_GEN_CONN_POOL_H */