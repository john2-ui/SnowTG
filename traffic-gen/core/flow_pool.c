#include "flow_pool.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int tg_flow_pool_index(const struct tg_flow_pool *pool,
                              const struct tg_flow *flow, uint32_t *index_out) {
        uintptr_t base;
        uintptr_t end;
        uintptr_t address;
        uintptr_t offset;

        if (pool == NULL || pool->flows == NULL || flow == NULL ||
            index_out == NULL)
                return -1;

        base = (uintptr_t)pool->flows;
        end = base + (uintptr_t)pool->capacity * sizeof(*pool->flows);
        address = (uintptr_t)flow;
        if (address < base || address >= end)
                return -1;

        offset = address - base;
        if (offset % sizeof(*pool->flows) != 0)
                return -1;

        *index_out = offset / sizeof(*pool->flows);
        return 0;
}

int tg_flow_pool_init(struct tg_flow_pool *pool, uint32_t capacity) {
        uint32_t i;

        if (pool == NULL || capacity == 0) {
                errno = EINVAL;
                return -1;
        }

        memset(pool, 0, sizeof(*pool));
        pool->flows = calloc(capacity, sizeof(*pool->flows));
        pool->free_ids = calloc(capacity, sizeof(*pool->free_ids));
        if (pool->flows == NULL || pool->free_ids == NULL) {
                free(pool->free_ids);
                free(pool->flows);
                memset(pool, 0, sizeof(*pool));
                errno = ENOMEM;
                return -1;
        }

        for (i = 0; i < capacity; i++) {
                tg_flow_reset(&pool->flows[i]);
                pool->free_ids[i] = capacity - 1U - i;
        }

        pool->capacity = capacity;
        pool->free_count = capacity;
        return 0;
}

void tg_flow_pool_fini(struct tg_flow_pool *pool) {
        if (pool == NULL)
                return;

        free(pool->free_ids);
        free(pool->flows);
        memset(pool, 0, sizeof(*pool));
}

struct tg_flow *tg_flow_pool_get(struct tg_flow_pool *pool) {
        struct tg_flow *flow;
        uint32_t id;

        if (pool == NULL || pool->flows == NULL || pool->free_ids == NULL) {
                errno = EINVAL;
                return NULL;
        }
        if (pool->free_count == 0) {
                errno = ENOBUFS;
                return NULL;
        }

        id = pool->free_ids[--pool->free_count];
        flow = &pool->flows[id];
        tg_flow_reset(flow);
        flow->in_use = true;
        return flow;
}

int tg_flow_pool_put(struct tg_flow_pool *pool, struct tg_flow *flow) {
        uint32_t id;

        if (pool == NULL || pool->flows == NULL || pool->free_ids == NULL ||
            tg_flow_pool_index(pool, flow, &id) != 0) {
                errno = EINVAL;
                return -1;
        }
        if (flow->mapped) {
                errno = EBUSY;
                return -1;
        }
        if (!flow->in_use) {
                errno = EALREADY;
                return -1;
        }
        if (pool->free_count >= pool->capacity) {
                errno = EOVERFLOW;
                return -1;
        }

        tg_flow_reset(flow);
        pool->free_ids[pool->free_count++] = id;
        return 0;
}
