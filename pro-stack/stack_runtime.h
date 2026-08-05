#ifndef NETARCH_STACK_RUNTIME_H
#define NETARCH_STACK_RUNTIME_H

/**
 * @file stack_runtime.h
 * @brief Owner-worker loop integration point for upper-layer reactors.
 */

#include <stdint.h>

struct rte_mempool;

typedef void (*stack_runtime_reactor_fn)(void *ctx, unsigned int budget);

/**
 * Register the owner-lcore reactor before launching the packet worker.
 * Passing NULL restores protocol-stack-only operation.
 */
void stack_runtime_set_reactor(stack_runtime_reactor_fn fn, void *ctx);

/** DPDK lcore entry point: packet ingress, timers, reactor, and TX flush. */
int stack_runtime_worker_entry(void *arg);

#endif /* NETARCH_STACK_RUNTIME_H */
