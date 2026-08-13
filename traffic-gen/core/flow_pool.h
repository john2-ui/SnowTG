#ifndef TRAFFIC_GEN_FLOW_POOL_H
#define TRAFFIC_GEN_FLOW_POOL_H

/**
 * @file flow_pool.h
 * @brief Owner-local fixed-capacity allocator for traffic-generator flows.
 *
 * The pool preallocates exactly the number of flow objects permitted by the
 * scenario's concurrency limit. A flow represents one physical connection;
 * an HTTP keep-alive flow can carry multiple sequential transactions.
 * This prevents allocator activity in the admission hot path.
 */

#include "flow.h"

#include <stdlib.h>

/**
 * @brief Stack-indexed storage and free-list metadata for @ref tg_flow items.
 *
 * Only the owning worker may get or put flows.  A returned flow is reset,
 * unmapped, and unavailable to any stale readiness event.
 */
struct tg_flow_pool {
        struct tg_flow *flows;
        uint32_t *free_ids;
        uint32_t capacity;
        uint32_t free_count;
};

/**
 * @brief Allocates and initializes a fixed number of reusable flow objects.
 * @param pool Destination pool.
 * @param capacity Number of simultaneously acquirable flows.
 * @return 0 on success; -1 with @c errno set on invalid input or allocation
 *         failure.
 */
int tg_flow_pool_init(struct tg_flow_pool *pool, uint32_t capacity);

/**
 * @brief Releases pool storage after all flows have been reclaimed.
 * @param pool Pool to destroy; @c NULL is accepted.
 */
void tg_flow_pool_fini(struct tg_flow_pool *pool);

/**
 * @brief Acquires one reset flow object from the owner-local free list.
 * @param pool Pool from which to obtain a flow.
 * @return A flow marked in use, or @c NULL with @c errno set on exhaustion.
 */
struct tg_flow *tg_flow_pool_get(struct tg_flow_pool *pool);

/**
 * @brief Resets and returns a non-mapped flow to its owning pool.
 * @param pool Pool that originally supplied @p flow.
 * @param flow Flow to reclaim.
 * @return 0 on success; -1 when ownership or lifecycle invariants fail.
 */
int tg_flow_pool_put(struct tg_flow_pool *pool, struct tg_flow *flow);

#endif /* TRAFFIC_GEN_FLOW_POOL_H */
