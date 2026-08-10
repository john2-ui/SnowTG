/**
 * @file scenario.c
 * @brief Compiles a strictly validated JSON scenario into a runtime plan.
 *
 * This is startup-only control-plane code.  It rejects unknown schema fields,
 * unsupported escaped strings, and out-of-range values so the owner-local hot
 * path can consume fixed-size, pointer-stable plan data without JSON parsing.
 */

#include "scenario.h"
#include "scenario_json.h"

#include "../proto/registry.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Largest accepted on-disk scenario document, in bytes. */
#define TG_SCENARIO_MAX_BYTES (64U * 1024U)
/** @brief Tokenization bound that limits parser stack and startup work. */
#define TG_SCENARIO_MAX_TOKENS 512U

/** @brief Releases one protocol-owned class configuration. */
static void tg_class_plan_fini(struct tg_class_plan *class_plan) {
        if (class_plan == NULL)
                return;
        if (class_plan->proto_config != NULL && class_plan->proto != NULL &&
            class_plan->proto->config_free != NULL)
                class_plan->proto->config_free(class_plan->proto_config);
        memset(class_plan, 0, sizeof(*class_plan));
}

/**
 * @brief Aborts a partially cloned shard without touching unreached classes.
 *
 * The class array is copied from the source plan before cloning begins, so
 * only the prefix through the failed clone may contain owned allocations.
 * Config pointers for that prefix are initialized before each clone; later
 * classes are deliberately outside this cleanup range.
 */
static void tg_plan_partition_abort(struct tg_plan *partitioned,
                                    uint32_t covered_class_count) {
        if (partitioned == NULL)
                return;
        if (covered_class_count > TG_PLAN_MAX_CLASSES)
                covered_class_count = TG_PLAN_MAX_CLASSES;
        for (uint32_t class_index = 0; class_index < covered_class_count;
             class_index++)
                tg_class_plan_fini(&partitioned->classes[class_index]);
        memset(partitioned, 0, sizeof(*partitioned));
}

/** @brief Validates and compiles an IPv4 peer object. */
static int tg_parse_peer(const struct tg_json_doc *doc, int object_index,
                         struct tg_class_plan *class_plan) {
        static const char *const allowed[] = {"ip", "port"};
        char ip[INET_ADDRSTRLEN];
        int ip_index;
        int port_index;
        uint32_t port;

        if (doc == NULL || doc->tokens == NULL || object_index < 0 ||
            object_index >= doc->token_count ||
            doc->tokens[object_index].type != JSMN_OBJECT ||
            !tg_json_object_has_only(doc, object_index, allowed,
                                     sizeof(allowed) / sizeof(allowed[0]))) {
                errno = EINVAL;
                return -1;
        }
        ip_index = tg_json_object_value(doc, object_index, "ip");
        port_index = tg_json_object_value(doc, object_index, "port");
        if (ip_index < 0 || port_index < 0 ||
            tg_json_copy_string(ip, sizeof(ip), doc, &doc->tokens[ip_index]) !=
                0 ||
            tg_json_parse_u32(doc, &doc->tokens[port_index], 1, UINT16_MAX,
                              &port) != 0) {
                errno = EINVAL;
                return -1;
        }

        memset(&class_plan->peer, 0, sizeof(class_plan->peer));
        class_plan->peer.sin_family = AF_INET;
        class_plan->peer.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, ip, &class_plan->peer.sin_addr) != 1) {
                errno = EINVAL;
                return -1;
        }
        return 0;
}

/** @brief Returns whether a class object key is common or registered. */
static bool tg_class_key_allowed(const struct tg_json_doc *doc,
                                 const jsmntok_t *key_token) {
        static const char *const common[] = {"name", "weight", "transport",
                                             "peer"};

        for (size_t index = 0; index < sizeof(common) / sizeof(common[0]);
             index++) {
                if (tg_json_token_equal(doc, key_token, common[index]))
                        return true;
        }
        return tg_proto_scenario_find_token(doc, key_token) != NULL;
}

/** @brief Reject unknown keys in one class while allowing registered plugins.
 */
static bool tg_class_has_only_allowed(const struct tg_json_doc *doc,
                                      int object_index) {
        if (doc == NULL || doc->tokens == NULL || object_index < 0 ||
            object_index >= doc->token_count)
                return false;
        for (int index = object_index + 1; index < doc->token_count; index++) {
                if (doc->tokens[index].parent != object_index)
                        continue;
                if (doc->tokens[index].type != JSMN_STRING ||
                    !tg_class_key_allowed(doc, &doc->tokens[index]))
                        return false;
        }
        return true;
}

/** @brief Converts a schema transport string to its runtime enum. */
static int tg_parse_transport(const struct tg_json_doc *doc,
                              const jsmntok_t *token,
                              enum tg_transport *transport_out) {
        if (transport_out == NULL) {
                errno = EINVAL;
                return -1;
        }
        if (tg_json_token_equal(doc, token, "tcp")) {
                *transport_out = TG_TRANSPORT_TCP;
                return 0;
        }
        if (tg_json_token_equal(doc, token, "udp")) {
                *transport_out = TG_TRANSPORT_UDP;
                return 0;
        }
        errno = EINVAL;
        return -1;
}

/**
 * @brief Compiles one complete traffic-class declaration.
 */
static int tg_parse_class(const struct tg_json_doc *doc, int object_index,
                          struct tg_class_plan *class_plan) {
        const struct tg_proto_scenario *protocol = NULL;
        int name_index;
        int weight_index;
        int transport_index;
        int peer_index;
        int protocol_index = -1;
        enum tg_transport transport;

        if (doc == NULL || doc->tokens == NULL || class_plan == NULL ||
            object_index < 0 || object_index >= doc->token_count ||
            doc->tokens[object_index].type != JSMN_OBJECT ||
            !tg_class_has_only_allowed(doc, object_index)) {
                errno = EINVAL;
                return -1;
        }

        name_index = tg_json_object_value(doc, object_index, "name");
        weight_index = tg_json_object_value(doc, object_index, "weight");
        transport_index = tg_json_object_value(doc, object_index, "transport");
        peer_index = tg_json_object_value(doc, object_index, "peer");
        for (size_t index = 0; index < tg_proto_scenario_count(); index++) {
                const struct tg_proto_scenario *candidate =
                    tg_proto_scenario_at(index);
                int candidate_index = tg_json_object_value(
                    doc, object_index, candidate->schema_key);

                if (candidate_index < 0)
                        continue;
                if (protocol != NULL) {
                        errno = EINVAL;
                        return -1;
                }
                protocol = candidate;
                protocol_index = candidate_index;
        }
        if (name_index < 0 || weight_index < 0 || transport_index < 0 ||
            peer_index < 0 || protocol == NULL ||
            tg_parse_transport(doc, &doc->tokens[transport_index],
                               &transport) != 0 ||
            transport != protocol->transport ||
            tg_json_copy_string(class_plan->name, sizeof(class_plan->name), doc,
                                &doc->tokens[name_index]) != 0 ||
            tg_json_parse_u32(doc, &doc->tokens[weight_index], 1, UINT32_MAX,
                              &class_plan->weight) != 0 ||
            tg_parse_peer(doc, peer_index, class_plan) != 0) {
                if (errno == 0)
                        errno = EINVAL;
                return -1;
        }
        class_plan->transport = transport;
        class_plan->proto = protocol->ops;
        if (protocol->compile == NULL ||
            protocol->compile(doc, protocol_index, class_plan) != 0) {
                if (errno == 0)
                        errno = EINVAL;
                return -1;
        }
        return 0;
}

/**
 * @brief Compiles the class array and validates its aggregate weight.
 */
static int tg_parse_classes(const struct tg_json_doc *doc, int array_index,
                            struct tg_plan *plan) {
        uint64_t total_weight = 0;

        if (doc == NULL || doc->tokens == NULL || plan == NULL ||
            array_index < 0 || array_index >= doc->token_count ||
            doc->tokens[array_index].type != JSMN_ARRAY ||
            doc->tokens[array_index].size == 0 ||
            doc->tokens[array_index].size > (int)TG_PLAN_MAX_CLASSES) {
                errno = EINVAL;
                return -1;
        }
        for (int i = array_index + 1; i < doc->token_count; i++) {
                struct tg_class_plan *class_plan;

                if (doc->tokens[i].parent != array_index)
                        continue;
                if (plan->class_count >= TG_PLAN_MAX_CLASSES)
                        return -1;
                class_plan = &plan->classes[plan->class_count];
                if (tg_parse_class(doc, i, class_plan) != 0) {
                        tg_class_plan_fini(class_plan);
                        return -1;
                }
                total_weight += plan->classes[plan->class_count].weight;
                if (total_weight > UINT32_MAX) {
                        errno = ERANGE;
                        tg_class_plan_fini(class_plan);
                        return -1;
                }
                plan->class_count++;
        }
        if (plan->class_count == 0) {
                errno = EINVAL;
                return -1;
        }
        plan->total_weight = (uint32_t)total_weight;
        return 0;
}

/**
 * @brief Reads a bounded scenario file into a temporary NUL-terminated buffer.
 */
static int tg_read_file(const char *path, char **json_out, size_t *length_out) {
        FILE *file;
        char *json;
        long length;

        file = fopen(path, "rb");
        if (file == NULL)
                return -1;
        if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
            (unsigned long)length > TG_SCENARIO_MAX_BYTES ||
            fseek(file, 0, SEEK_SET) != 0) {
                int saved_errno = errno == 0 ? EINVAL : errno;

                fclose(file);
                errno = saved_errno;
                return -1;
        }
        json = calloc((size_t)length + 1U, 1);
        if (json == NULL) {
                fclose(file);
                return -1;
        }
        if (fread(json, 1, (size_t)length, file) != (size_t)length ||
            fclose(file) != 0) {
                int saved_errno = errno == 0 ? EIO : errno;

                free(json);
                errno = saved_errno;
                return -1;
        }
        *json_out = json;
        *length_out = (size_t)length;
        return 0;
}

/** @copydoc tg_plan_load_file */
int tg_plan_load_file(struct tg_plan *plan, const char *path) {
        static const char *const allowed[] = {
            "name",       "duration_sec",        "max_concurrency",
            "target_cps", "report_interval_sec", "classes"};
        jsmntok_t tokens[TG_SCENARIO_MAX_TOKENS];
        jsmn_parser parser;
        struct tg_json_doc doc;
        char *json = NULL;
        size_t length;
        int token_count;
        int name_index;
        int duration_index;
        int concurrency_index;
        int cps_index;
        int report_index;
        int classes_index;
        int result = -1;

        if (plan == NULL || path == NULL) {
                errno = EINVAL;
                return -1;
        }
        memset(plan, 0, sizeof(*plan));
        if (tg_read_file(path, &json, &length) != 0)
                return -1;

        jsmn_init(&parser);
        token_count =
            jsmn_parse(&parser, json, length, tokens, TG_SCENARIO_MAX_TOKENS);
        doc.text = json;
        doc.tokens = tokens;
        doc.token_count = token_count;
        if (token_count <= 0 || tokens[0].type != JSMN_OBJECT ||
            !tg_json_object_has_only(&doc, 0, allowed,
                                     sizeof(allowed) / sizeof(allowed[0]))) {
                errno = EINVAL;
                goto out;
        }
        name_index = tg_json_object_value(&doc, 0, "name");
        duration_index = tg_json_object_value(&doc, 0, "duration_sec");
        concurrency_index = tg_json_object_value(&doc, 0, "max_concurrency");
        cps_index = tg_json_object_value(&doc, 0, "target_cps");
        report_index = tg_json_object_value(&doc, 0, "report_interval_sec");
        classes_index = tg_json_object_value(&doc, 0, "classes");
        if (name_index < 0 || duration_index < 0 || concurrency_index < 0 ||
            cps_index < 0 || classes_index < 0 ||
            tg_json_copy_string(plan->name, sizeof(plan->name), &doc,
                                &tokens[name_index]) != 0 ||
            tg_json_parse_u32(&doc, &tokens[duration_index], 1,
                              TG_PLAN_MAX_DURATION_SEC,
                              &plan->duration_sec) != 0 ||
            tg_json_parse_u32(&doc, &tokens[concurrency_index], 1,
                              TG_PLAN_MAX_CONCURRENCY,
                              &plan->max_concurrency) != 0 ||
            tg_json_parse_u32(&doc, &tokens[cps_index], 1, TG_PLAN_MAX_CPS,
                              &plan->target_cps) != 0 ||
            (report_index >= 0 &&
             tg_json_parse_u32(&doc, &tokens[report_index], 1,
                               TG_PLAN_MAX_REPORT_INTERVAL_SEC,
                               &plan->report_interval_sec) != 0) ||
            tg_parse_classes(&doc, classes_index, plan) != 0)
                goto out;
        if (report_index < 0)
                plan->report_interval_sec = 1;
        result = 0;
out:
        free(json);
        if (result != 0)
                tg_plan_fini(plan);
        return result;
}

unsigned int tg_plan_active_shards(const struct tg_plan *plan,
                                   unsigned int worker_count) {
        unsigned int active_shards;

        if (plan == NULL || worker_count == 0 || plan->target_cps == 0 ||
            plan->max_concurrency == 0)
                return 0;

        active_shards = worker_count;
        if (active_shards > plan->target_cps)
                active_shards = plan->target_cps;
        if (active_shards > plan->max_concurrency)
                active_shards = plan->max_concurrency;
        return active_shards;
}

int tg_plan_partition(struct tg_plan *destination, const struct tg_plan *source,
                      unsigned int shard_index, unsigned int shard_count) {
        struct tg_plan partitioned;
        uint32_t cps_base;
        uint32_t cps_remainder;
        uint32_t concurrency_base;
        uint32_t concurrency_remainder;

        if (destination == NULL || source == NULL || shard_count == 0 ||
            shard_index >= shard_count || destination == source ||
            source->target_cps == 0 || source->max_concurrency == 0 ||
            source->class_count == 0 || source->total_weight == 0 ||
            source->class_count > TG_PLAN_MAX_CLASSES ||
            shard_count > source->target_cps ||
            shard_count > source->max_concurrency) {
                errno = EINVAL;
                return -1;
        }

        memset(&partitioned, 0, sizeof(partitioned));
        memcpy(&partitioned, source, sizeof(partitioned));
        for (uint32_t class_index = 0; class_index < partitioned.class_count;
             class_index++)
                partitioned.classes[class_index].proto_config = NULL;

        cps_base = source->target_cps / shard_count;
        cps_remainder = source->target_cps % shard_count;
        concurrency_base = source->max_concurrency / shard_count;
        concurrency_remainder = source->max_concurrency % shard_count;
        partitioned.target_cps =
            cps_base + (shard_index < cps_remainder ? 1U : 0U);
        partitioned.max_concurrency =
            concurrency_base + (shard_index < concurrency_remainder ? 1U : 0U);
        partitioned.selection_phase =
            (uint32_t)(((uint64_t)shard_index * source->total_weight) /
                       shard_count);
        for (uint32_t class_index = 0; class_index < partitioned.class_count;
             class_index++) {
                const struct tg_class_plan *source_class =
                    &source->classes[class_index];
                struct tg_class_plan *destination_class =
                    &partitioned.classes[class_index];

                if (source_class->proto_config == NULL)
                        continue;
                if (source_class->proto == NULL ||
                    source_class->proto->config_clone == NULL ||
                    source_class->proto->config_free == NULL ||
                    source_class->proto->config_clone(
                        source_class->proto_config,
                        &destination_class->proto_config) != 0) {
                        if (errno == 0)
                                errno = EINVAL;
                        int saved_errno = errno;
                        tg_plan_partition_abort(&partitioned, class_index + 1U);
                        errno = saved_errno;
                        return -1;
                }
        }

        *destination = partitioned;
        return 0;
}

/** @copydoc tg_plan_fini */
void tg_plan_fini(struct tg_plan *plan) {
        if (plan == NULL)
                return;
        for (uint32_t class_index = 0; class_index < plan->class_count &&
                                       class_index < TG_PLAN_MAX_CLASSES;
             class_index++)
                tg_class_plan_fini(&plan->classes[class_index]);
        memset(plan, 0, sizeof(*plan));
}
