#ifndef TRAFFIC_GEN_CONN_POOL_H
#define TRAFFIC_GEN_CONN_POOL_H

/**
 * @file conn_pool.h
 * @brief Owner-local pool of reusable TCP traffic-generator connections.
 *
 * HTTP/1.1 keep-alive uses one in-flight transaction per connection. The
 * pool only selects idle connections; transport ownership and readiness stay
 * in core/flow.c.
 */

#include "scenario.h"

#include <stdbool.h>
#include <stdint.h>

struct tg_flow;

struct tg_conn_pool {
        struct tg_flow *idle_heads[TG_PLAN_MAX_CLASSES];
        const struct tg_class_plan *class_keys[TG_PLAN_MAX_CLASSES];
        uint32_t max_connections;
        uint32_t connections;
        bool draining;
};

/** Initialize an owner-local connection pool with a hard connection cap. */
int tg_conn_pool_init(struct tg_conn_pool *pool, uint32_t max_connections);

/** Clear pool metadata after all connections have been closed. */
void tg_conn_pool_fini(struct tg_conn_pool *pool);

/** Return whether a new connection may be attached to the pool. */
bool tg_conn_pool_can_create(const struct tg_conn_pool *pool);

/** Attach one newly admitted flow to a class-specific pool. */
int tg_conn_pool_attach(struct tg_conn_pool *pool, struct tg_flow *flow,
                        const struct tg_class_plan *class_plan);

/** Remove one flow from the pool, including any idle-list membership. */
void tg_conn_pool_detach(struct tg_conn_pool *pool, struct tg_flow *flow);

/** Take an idle connection matching the immutable class plan, if available. */
struct tg_flow *tg_conn_pool_take_idle(struct tg_conn_pool *pool,
                                       const struct tg_class_plan *class_plan);

/** Take any idle connection, used when beginning shutdown drain. */
struct tg_flow *tg_conn_pool_take_any_idle(struct tg_conn_pool *pool);

/** Return a reusable flow to its class-specific idle list. */
int tg_conn_pool_put_idle(struct tg_conn_pool *pool, struct tg_flow *flow);

/** Stop new reuse and new connection admission for this pool. */
void tg_conn_pool_begin_drain(struct tg_conn_pool *pool);

#endif /* TRAFFIC_GEN_CONN_POOL_H */
