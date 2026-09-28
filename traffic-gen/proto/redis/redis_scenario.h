#ifndef TRAFFIC_GEN_REDIS_SCENARIO_H
#define TRAFFIC_GEN_REDIS_SCENARIO_H

#include "../../core/scenario_json.h"
struct tg_class_plan;

int tg_redis_scenario_compile(const struct tg_json_doc *doc, int object_index,
                              struct tg_class_plan *class_plan);

#endif
