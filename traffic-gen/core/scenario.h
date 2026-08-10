#ifndef TRAFFIC_GEN_SCENARIO_H
#define TRAFFIC_GEN_SCENARIO_H

/**
 * @file scenario.h
 * @brief JSON scenario compiler and immutable runtime-plan definitions.
 *
 * Scenario input is accepted only during process startup.  The compiler
 * validates the supported schema and copies every hot-path value into a
 * @ref tg_plan, which is thereafter read-only for the lifetime of a shard.
 */

#include "../proto/proto.h"

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Maximum number of independently weighted traffic classes. */
#define TG_PLAN_MAX_CLASSES 16U
/** @brief Capacity, including NUL terminator, of plan and class names. */
#define TG_PLAN_CLASS_NAME_CAP 64U
/** Largest immutable serialized request retained once per traffic class. */
#define TG_PLAN_REQUEST_TEMPLATE_CAP 1024U
/** @brief Largest flow-pool and scheduler concurrency supported by a plan. */
#define TG_PLAN_MAX_CONCURRENCY 65536U
/** @brief Largest supported connection-start rate in attempts per second. */
#define TG_PLAN_MAX_CPS 1000000U
/** @brief Longest period during which a plan may admit new transactions. */
#define TG_PLAN_MAX_DURATION_SEC 86400U
/** @brief Longest supported interval between periodic statistics reports. */
#define TG_PLAN_MAX_REPORT_INTERVAL_SEC 3600U

/** @brief Transport adapters supported by the current scenario schema. */
enum tg_transport {
        /** @brief Nonblocking IPv4 TCP flow handled by core/flow.c. */
        TG_TRANSPORT_TCP = 0,
        /** @brief Owner-local IPv4 UDP datagram flow handled by core/flow.c. */
        TG_TRANSPORT_UDP,
};

/**
 * @brief Precompiled behavior and destination for one traffic class.
 *
 * Protocol-specific configuration is owned through @c proto_config.  Do not
 * point it at the transient JSON parsing buffer.
 */
struct tg_class_plan {
        char name[TG_PLAN_CLASS_NAME_CAP];
        uint32_t weight;
        enum tg_transport transport;
        struct sockaddr_in peer;

        const struct tg_proto_ops *proto;
        /**
         * Stable plugin configuration selected by the class parser.
         *
         * The pointed-to object is immutable on the hot path and owned by
         * the protocol plugin's config_free callback.
         */
        void *proto_config;
        uint8_t request_template[TG_PLAN_REQUEST_TEMPLATE_CAP];
        size_t request_template_len;
};

/**
 * @brief Complete validated plan consumed by one owner-local scheduler.
 *
 * The plan contains no parser-owned or socket-owned state.  It may therefore
 * be shared as immutable configuration once multi-shard execution is added.
 */
struct tg_plan {
        char name[TG_PLAN_CLASS_NAME_CAP];
        uint32_t duration_sec;
        uint32_t max_concurrency;
        uint32_t target_cps;
        uint32_t report_interval_sec;
        uint32_t total_weight;
        /** Initial weighted-round-robin phase for this scheduling shard. */
        uint32_t selection_phase;
        uint32_t class_count;
        struct tg_class_plan classes[TG_PLAN_MAX_CLASSES];
};

/**
 * @brief Reads, validates, and compiles a JSON scenario file.
 * @param plan Destination plan, cleared if compilation fails.
 * @param path Path to the scenario JSON document.
 * @return 0 on success; -1 with @c errno set on I/O, syntax, or schema error.
 */
int tg_plan_load_file(struct tg_plan *plan, const char *path);
/**
 * Return the number of active scheduling shards for a worker count.
 *
 * A worker with no CPS or concurrency share is not initialized as a scheduler.
 * The returned value is zero when the inputs cannot produce an active shard.
 */
unsigned int tg_plan_active_shards(const struct tg_plan *plan,
                                   unsigned int worker_count);
/**
 * Copy an immutable source plan into one scheduling shard while preserving the
 * source plan's global CPS and concurrency totals across all shards.
 *
 * The destination must be zero-initialized or already finalized, and must not
 * alias source.  Protocol-owned configuration is cloned through its generic
 * protocol contract.
 */
int tg_plan_partition(struct tg_plan *destination, const struct tg_plan *source,
                      unsigned int shard_index, unsigned int shard_count);

/**
 * @brief Releases protocol-owned configuration and clears plan state.
 * @param plan Plan to clear; @c NULL is accepted.
 */
void tg_plan_fini(struct tg_plan *plan);

#endif /* TRAFFIC_GEN_SCENARIO_H */
