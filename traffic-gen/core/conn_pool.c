/**
 * @file conn_pool.c
 * @brief Implements owner-local class-keyed idle connection lists.
 */

#include "conn_pool.h"

#include "flow.h"

#include <errno.h>
#include <string.h>

static int tg_conn_pool_class_index(struct tg_conn_pool *pool,
                                    const struct tg_class_plan *class_plan,
                                    bool create) {
        int free_index = -1;

        if (pool == NULL || class_plan == NULL)
                return -1;
        for (unsigned int index = 0; index < TG_PLAN_MAX_CLASSES; index++) {
                if (pool->class_keys[index] == class_plan)
                        return (int)index;
                if (free_index < 0 && pool->class_keys[index] == NULL)
                        free_index = (int)index;
        }
        if (!create || free_index < 0)
                return -1;
        pool->class_keys[free_index] = class_plan;
        return free_index;
}

int tg_conn_pool_init(struct tg_conn_pool *pool, uint32_t max_connections) {
        if (pool == NULL || max_connections == 0) {
                errno = EINVAL;
                return -1;
        }

        memset(pool, 0, sizeof(*pool));
        pool->max_connections = max_connections;
        return 0;
}

void tg_conn_pool_fini(struct tg_conn_pool *pool) {
        if (pool == NULL)
                return;
        memset(pool, 0, sizeof(*pool));
}

bool tg_conn_pool_can_create(const struct tg_conn_pool *pool) {
        return pool != NULL && !pool->draining &&
               pool->connections < pool->max_connections;
}

int tg_conn_pool_attach(struct tg_conn_pool *pool, struct tg_flow *flow,
                        const struct tg_class_plan *class_plan) {
        if (pool == NULL || flow == NULL || class_plan == NULL ||
            !tg_conn_pool_can_create(pool)) {
                errno = EAGAIN;
                return -1;
        }
        if (flow->conn_pool != NULL) {
                errno = EALREADY;
                return -1;
        }
        if (tg_conn_pool_class_index(pool, class_plan, true) < 0) {
                errno = ENOSPC;
                return -1;
        }

        flow->conn_pool = pool;
        flow->class_plan = class_plan;
        flow->pool_next = NULL;
        flow->in_idle_pool = false;
        pool->connections++;
        return 0;
}

void tg_conn_pool_detach(struct tg_conn_pool *pool, struct tg_flow *flow) {
        int class_index;
        struct tg_flow **cursor;

        if (pool == NULL || flow == NULL || flow->conn_pool != pool)
                return;

        class_index = tg_conn_pool_class_index(pool, flow->class_plan, false);
        if (flow->in_idle_pool && class_index >= 0) {
                cursor = &pool->idle_heads[class_index];
                while (*cursor != NULL && *cursor != flow)
                        cursor = &(*cursor)->pool_next;
                if (*cursor == flow)
                        *cursor = flow->pool_next;
        }
        flow->pool_next = NULL;
        flow->in_idle_pool = false;
        flow->conn_pool = NULL;
        if (pool->connections != 0)
                pool->connections--;
        if (class_index >= 0 && pool->idle_heads[class_index] == NULL)
                pool->class_keys[class_index] = NULL;
}

struct tg_flow *tg_conn_pool_take_idle(
    struct tg_conn_pool *pool, const struct tg_class_plan *class_plan) {
        int class_index;
        struct tg_flow *flow;

        if (pool == NULL || pool->draining || class_plan == NULL)
                return NULL;
        class_index = tg_conn_pool_class_index(pool, class_plan, false);
        if (class_index < 0)
                return NULL;
        flow = pool->idle_heads[class_index];
        if (flow == NULL)
                return NULL;
        pool->idle_heads[class_index] = flow->pool_next;
        flow->pool_next = NULL;
        flow->in_idle_pool = false;
        return flow;
}

struct tg_flow *tg_conn_pool_take_any_idle(struct tg_conn_pool *pool) {
        if (pool == NULL)
                return NULL;
        for (unsigned int index = 0; index < TG_PLAN_MAX_CLASSES; index++) {
                struct tg_flow *flow = pool->idle_heads[index];

                if (flow == NULL)
                        continue;
                pool->idle_heads[index] = flow->pool_next;
                flow->pool_next = NULL;
                flow->in_idle_pool = false;
                return flow;
        }
        return NULL;
}

int tg_conn_pool_put_idle(struct tg_conn_pool *pool, struct tg_flow *flow) {
        int class_index;

        if (pool == NULL || flow == NULL || flow->conn_pool != pool ||
            flow->class_plan == NULL || flow->in_idle_pool) {
                errno = EINVAL;
                return -1;
        }
        if (pool->draining) {
                errno = ESHUTDOWN;
                return -1;
        }
        class_index = tg_conn_pool_class_index(pool, flow->class_plan, true);
        if (class_index < 0) {
                errno = ENOSPC;
                return -1;
        }
        flow->pool_next = pool->idle_heads[class_index];
        pool->idle_heads[class_index] = flow;
        flow->in_idle_pool = true;
        return 0;
}

void tg_conn_pool_begin_drain(struct tg_conn_pool *pool) {
        if (pool != NULL)
                pool->draining = true;
}
