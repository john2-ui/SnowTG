#ifndef TRAFFIC_GEN_PROTO_REGISTRY_H
#define TRAFFIC_GEN_PROTO_REGISTRY_H

/**
 * @file registry.h
 * @brief Compile-time registry for protocol-specific scenario objects.
 *
 * Runtime byte processing remains described by tg_proto_ops.  This registry
 * adds the control-plane metadata needed to select and compile one protocol
 * object from a class without placing protocol-specific branches in
 * core/scenario.c.
 */

#include "../core/scenario.h"
#include "../core/scenario_json.h"

#include <stddef.h>

typedef int (*tg_proto_scenario_compile_fn)(const struct tg_json_doc *doc,
                                            int object_index,
                                            struct tg_class_plan *class_plan);

struct tg_proto_scenario {
        const char *schema_key;
        const struct tg_proto_ops *ops;
        enum tg_transport transport;
        tg_proto_scenario_compile_fn compile;
};

/** Return the number of statically registered scenario protocols. */
size_t tg_proto_scenario_count(void);

/** Return the descriptor at @p index, or NULL when out of range. */
const struct tg_proto_scenario *tg_proto_scenario_at(size_t index);

/**
 * Find a descriptor by its JSON key token.
 *
 * @return Matching descriptor, or NULL when the token is not a registered
 * protocol key.
 */
const struct tg_proto_scenario *
tg_proto_scenario_find_token(const struct tg_json_doc *doc,
                             const jsmntok_t *key_token);

#endif /* TRAFFIC_GEN_PROTO_REGISTRY_H */
