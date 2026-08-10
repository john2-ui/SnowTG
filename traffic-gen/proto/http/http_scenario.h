#ifndef TRAFFIC_GEN_HTTP_SCENARIO_H
#define TRAFFIC_GEN_HTTP_SCENARIO_H

/**
 * @file http_scenario.h
 * @brief HTTP-specific scenario object compiler.
 */

#include "../../core/scenario_json.h"

struct tg_class_plan;

/**
 * Compile one `http` object into an owning HTTP configuration and request
 * template on a class plan whose protocol operations are already selected.
 */
int tg_http_scenario_compile(const struct tg_json_doc *doc, int object_index,
                             struct tg_class_plan *class_plan);

#endif /* TRAFFIC_GEN_HTTP_SCENARIO_H */
