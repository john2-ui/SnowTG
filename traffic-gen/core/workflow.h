#ifndef TG_WORKFLOW_H
#define TG_WORKFLOW_H

/**
 * @file workflow.h
 * @brief Owner-local execution and statistics for multi-step business transactions.
 *
 * Each admitted transaction retains its concurrency slot across network, branch,
 * and think steps. Flow callbacks export results and enqueue work; only the owner
 * tick advances steps. Main may inspect counters after all workers have joined.
 */

#include "../../pro-stack/owner_timer.h"
#include "conn_pool.h"
#include "flow_pool.h"
#include "latency.h"
#include "stats.h"
#include "workflow_plan.h"
struct tg_business;

/**
 * @brief Terminal accounting for one phase/class/step.
 *
 * After drain: reached = started + start_failed = success + failed.
 * The internal failed count includes start_failed; CSV serialization separates
 * the two. reached + branch_skipped + not_reached equals parent admissions.
 */
struct tg_step_counts {
        uint64_t reached, started, success, failed, start_failed,
            branch_skipped, not_reached;
};

/**
 * @brief One owner's counters and optional complete/success/failure histograms.
 */
struct tg_step_stats {
        struct tg_step_counts counts;
        struct tg_histogram *hist;
};

/**
 * @brief Bounded transaction storage and borrowed shard facilities.
 *
 * The engine owns storage, steps, and histograms. Plan, Flow/socket facilities,
 * scheduler, and base statistics belong to the shard and must outlive it.
 */
struct tg_workflow_engine {
        const struct tg_plan *plan;
        struct tg_flow_map *map;
        struct tg_flow_pool *flows;
        struct tg_conn_pool *connections;
        struct tg_scheduler *scheduler;
        struct tg_stats *stats;
        struct tg_latency *latency;
        tg_flow_socket_created_fn created;
        owner_io_release_fn released;
        void *socket_ctx;
        /* Free-list and ready-queue linkage are mutually exclusive. */
        struct tg_business *storage, *free, *head, *tail;
        struct tg_step_stats *steps;
        struct tg_histogram *histograms;
        /* Per-class step offsets within each phase; capacity is shard-local. */
        unsigned offsets[TG_PLAN_MAX_CLASSES], step_count, capacity;
        uint64_t memory_bytes;
};

/**
 * @brief Allocates per-owner business objects and optional step histograms.
 *
 * Call before worker launch. Legacy-only plans allocate no business storage.
 * @return 0 on success; -1 after rolling back any partial allocation.
 */
int tg_workflow_engine_init(struct tg_workflow_engine *e);

/**
 * @brief Releases engine-owned storage after worker shutdown.
 *
 * This does not complete active transactions or drain sockets; the caller must
 * finish those lifecycle operations before destroying the engine.
 */
void tg_workflow_engine_fini(struct tg_workflow_engine *e);

/**
 * @brief Admits one business transaction and enqueues its first step.
 *
 * The scheduler supplies the planned ordinal and phase and increments active
 * only after this returns success. No network request is started inline.
 * @return 0 on admission; -1 when storage or timer registration fails.
 */
int tg_workflow_start(struct tg_workflow_engine *e,
                      const struct tg_class_plan *c);

/**
 * @brief Advances at most budget ready entries on the owning worker.
 *
 * Call after Flow event callbacks return, including during admission shutdown
 * while existing transactions still have later steps to execute.
 */
void tg_workflow_tick(struct tg_workflow_engine *e, unsigned budget);

/**
 * @brief Merges owner step counters and histogram buckets after worker join.
 *
 * Step rows use their own CSV scope and must not be added to transaction rows.
 * @return 0 on success; -1 on allocation or stream failure.
 */
int tg_workflow_csv(FILE *f, const struct tg_plan *plan,
                    struct tg_workflow_engine *const *engines, unsigned count);

/**
 * @brief Serializes one merged step metric using the common histogram format.
 * @return 0 on success; -1 on allocation or stream failure.
 */
int tg_latency_csv_step(FILE *f, const char *phase, const char *cls,
                        const char *protocol, const char *step,
                        const char *metric, const struct tg_histogram *hist,
                        const struct tg_step_counts *counts);
#endif
