/**
 * @file registry.c
 * @brief Implements the traffic-generator scenario protocol registry.
 */

#include "registry.h"

#include "dns/dns_client.h"
#include "dns/dns_scenario.h"
#include "http/http_client.h"
#include "http/http_scenario.h"

static const struct tg_proto_scenario tg_proto_scenarios[] = {
    {
        .schema_key = "http",
        .ops = &tg_http_proto_ops,
        .transport = TG_TRANSPORT_TCP,
        .compile = tg_http_scenario_compile,
    },
    {
        .schema_key = "dns",
        .ops = &tg_dns_proto_ops,
        .transport = TG_TRANSPORT_UDP,
        .compile = tg_dns_scenario_compile,
    },
};

/** @copydoc tg_proto_scenario_count */
size_t tg_proto_scenario_count(void) {
        return sizeof(tg_proto_scenarios) / sizeof(tg_proto_scenarios[0]);
}

/** @copydoc tg_proto_scenario_at */
const struct tg_proto_scenario *tg_proto_scenario_at(size_t index) {
        if (index >= tg_proto_scenario_count())
                return NULL;
        return &tg_proto_scenarios[index];
}

/** @copydoc tg_proto_scenario_find_token */
const struct tg_proto_scenario *
tg_proto_scenario_find_token(const struct tg_json_doc *doc,
                             const jsmntok_t *key_token) {
        for (size_t index = 0; index < tg_proto_scenario_count(); index++) {
                const struct tg_proto_scenario *scenario =
                    &tg_proto_scenarios[index];

                if (tg_json_token_equal(doc, key_token, scenario->schema_key))
                        return scenario;
        }
        return NULL;
}
