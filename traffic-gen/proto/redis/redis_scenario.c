#include "redis_scenario.h"
#include "redis_client.h"
#include "../../core/scenario.h"
#include "../../core/value.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int tg_redis_scenario_compile(const struct tg_json_doc *doc, int object_index,
                              struct tg_class_plan *class_plan) {
        static const char *const allowed[] = {"command", "key", "value", "keepalive"};
        char command[5];
        if (!doc || !doc->tokens || !class_plan || object_index < 0 ||
            object_index >= doc->token_count ||
            doc->tokens[object_index].type != JSMN_OBJECT ||
            !tg_json_object_has_only(doc, object_index, allowed, 4)) {
                errno = EINVAL;
                return -1;
        }
        struct tg_redis_config *config = calloc(1, sizeof(*config));
        if (!config)
                return -1;
        config->keepalive = true;
        int cmd = tg_json_object_value(doc, object_index, "command");
        int key = tg_json_object_value(doc, object_index, "key");
        int value = tg_json_object_value(doc, object_index, "value");
        int keepalive = tg_json_object_value(doc, object_index, "keepalive");
        if (tg_string_read(doc, cmd, command, sizeof(command)))
                goto invalid;
        for (size_t i = 0; command[i]; i++)
                if (command[i] >= 'a' && command[i] <= 'z')
                        command[i] -= 'a' - 'A';
        if (!strcmp(command, "PING"))
                config->command = TG_REDIS_PING;
        else if (!strcmp(command, "GET"))
                config->command = TG_REDIS_GET;
        else if (!strcmp(command, "SET"))
                config->command = TG_REDIS_SET;
        else
                goto invalid;
        if ((key >= 0) != (config->command != TG_REDIS_PING) ||
            (value >= 0) != (config->command == TG_REDIS_SET) ||
            (key >= 0 && tg_string_read(doc, key, config->key, sizeof(config->key))) ||
            (value >= 0 && tg_string_read(doc, value, config->value, sizeof(config->value))) ||
            (keepalive >= 0 && tg_json_parse_bool(doc, &doc->tokens[keepalive],
                                                 &config->keepalive)))
                goto invalid;
        if (tg_redis_proto_ops.build_request(config, class_plan->request_template,
                sizeof(class_plan->request_template), &class_plan->request_template_len)) {
                free(config);
                return -1;
        }
        class_plan->proto = &tg_redis_proto_ops;
        class_plan->proto_config = config;
        return 0;
invalid:
        free(config);
        errno = EINVAL;
        return -1;
}
