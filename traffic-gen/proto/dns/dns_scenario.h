#ifndef TRAFFIC_GEN_DNS_SCENARIO_H
#define TRAFFIC_GEN_DNS_SCENARIO_H

/**
 * @file dns_scenario.h
 * @brief DNS-specific scenario object compiler.
 */

#include "../../core/scenario_json.h"

struct tg_class_plan;

/**
 * Compile one `dns` object into an owning DNS configuration and request
 * template on a class plan whose protocol operations are already selected.
 */
int tg_dns_scenario_compile(const struct tg_json_doc *doc, int object_index,
                            struct tg_class_plan *class_plan);

#endif /* TRAFFIC_GEN_DNS_SCENARIO_H */
