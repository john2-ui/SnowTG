#ifndef TRAFFIC_GEN_SOCKET_CAPACITY_H
#define TRAFFIC_GEN_SOCKET_CAPACITY_H

/**
 * @file socket_capacity.h
 * @brief Startup policy for sizing per-owner socket resources.
 */

#include "scenario.h"

#include <stdint.h>

/**
 * Calculate the per-owner socket capacity needed by a loaded plan.
 *
 * Active traffic concurrency is not the full socket lifetime: SYN retries and
 * TCP teardown can retain owner slots after a flow leaves the scheduler.
 * Automatic sizing reserves at least 16384 slots, or twice the per-shard
 * active concurrency for larger plans.  An explicit override may reduce
 * that default, but must still cover both the stack's legacy minimum and
 * twice the per-shard concurrency.  Tables are allocated once at startup;
 * this policy does not resize live owner resources.
 *
 * @param plan Loaded immutable traffic plan.
 * @param active_shards Number of scheduling shards.
 * @param override Explicit capacity, or zero for automatic sizing.
 * @param capacity_out Receives the selected per-owner capacity.
 * @return 0 on success, -1 with errno set otherwise.
 */
int tg_socket_id_capacity(const struct tg_plan *plan,
                          unsigned int active_shards, uint32_t override,
                          uint32_t *capacity_out);

#endif /* TRAFFIC_GEN_SOCKET_CAPACITY_H */
