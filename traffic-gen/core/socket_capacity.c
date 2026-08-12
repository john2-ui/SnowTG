/**
 * @file socket_capacity.c
 * @brief Computes startup-time per-owner socket resource capacities.
 */

#include "socket_capacity.h"

#include "../../pro-stack/socket_owner.h"

#include <errno.h>
#include <rte_hash.h>
#include <stdint.h>

/** @brief Reserve factor for sockets that outlive active traffic flows. */
#define TG_SOCKET_ID_RESERVE_FACTOR 2U

/** @copydoc tg_socket_id_capacity */
int tg_socket_id_capacity(const struct tg_plan *plan,
                          unsigned int active_shards, uint32_t override,
                          uint32_t *capacity_out) {
        uint64_t per_shard;
        uint64_t required;

        if (plan == NULL || active_shards == 0 || capacity_out == NULL) {
                errno = EINVAL;
                return -1;
        }
        per_shard = ((uint64_t)plan->max_concurrency + active_shards - 1U) /
                    active_shards;
        required = per_shard * TG_SOCKET_ID_RESERVE_FACTOR;
        if (required < NSOCK_ID_DEFAULT_CAPACITY)
                required = NSOCK_ID_DEFAULT_CAPACITY;
        if (required > UINT32_MAX) {
                errno = EOVERFLOW;
                return -1;
        }
        if (required > RTE_HASH_ENTRIES_MAX) {
                errno = ERANGE;
                return -1;
        }
        *capacity_out = (uint32_t)required;
        if (override != 0) {
                if ((uint64_t) override < required) {
                        errno = ERANGE;
                        return -1;
                }
                if (override > RTE_HASH_ENTRIES_MAX) {
                        errno = ERANGE;
                        return -1;
                }
                *capacity_out = override;
        }
        return 0;
}
