/**
 * @file http_scenario.c
 * @brief Compiles the HTTP object in a traffic-generator scenario.
 */

#include "http_scenario.h"

#include "../../core/scenario.h"
#include "http_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/** @copydoc tg_http_scenario_compile */
int tg_http_scenario_compile(const struct tg_json_doc *doc, int object_index,
                             struct tg_class_plan *class_plan) {
        static const char *const allowed[] = {"method", "path", "host",
                                              "keepalive"};
        struct tg_http_config *config;
        int method_index;
        int path_index;
        int host_index;
        int keepalive_index;
        bool keepalive = false;

        if (doc == NULL || doc->tokens == NULL || class_plan == NULL ||
            object_index < 0 || object_index >= doc->token_count ||
            doc->tokens[object_index].type != JSMN_OBJECT ||
            !tg_json_object_has_only(doc, object_index, allowed,
                                     sizeof(allowed) / sizeof(allowed[0]))) {
                errno = EINVAL;
                return -1;
        }
        config = calloc(1, sizeof(*config));
        if (config == NULL)
                return -1;

        method_index = tg_json_object_value(doc, object_index, "method");
        path_index = tg_json_object_value(doc, object_index, "path");
        host_index = tg_json_object_value(doc, object_index, "host");
        keepalive_index = tg_json_object_value(doc, object_index, "keepalive");
        if (method_index < 0 || path_index < 0 ||
            tg_json_copy_string(config->method, sizeof(config->method), doc,
                                &doc->tokens[method_index]) != 0 ||
            tg_json_copy_string(config->path, sizeof(config->path), doc,
                                &doc->tokens[path_index]) != 0 ||
            config->path[0] != '/') {
                free(config);
                errno = EINVAL;
                return -1;
        }
        if (host_index >= 0) {
                if (tg_json_copy_string(config->host, sizeof(config->host),
                                        doc, &doc->tokens[host_index]) != 0 ||
                    config->host[0] == '\0' ||
                    strchr(config->host, '\r') != NULL ||
                    strchr(config->host, '\n') != NULL) {
                        free(config);
                        errno = EINVAL;
                        return -1;
                }
        } else if (inet_ntop(AF_INET, &class_plan->peer.sin_addr,
                             config->host, sizeof(config->host)) == NULL) {
                free(config);
                return -1;
        }
        if (keepalive_index >= 0 &&
            tg_json_parse_bool(doc, &doc->tokens[keepalive_index],
                               &keepalive) != 0) {
                free(config);
                return -1;
        }

        config->connection_close = !keepalive;
        class_plan->proto = &tg_http_proto_ops;
        class_plan->proto_config = config;
        if (class_plan->proto->build_request(
                config, class_plan->request_template,
                sizeof(class_plan->request_template),
                &class_plan->request_template_len) != 0 ||
            class_plan->request_template_len == 0) {
                int saved_errno = errno == 0 ? EINVAL : errno;

                class_plan->proto_config = NULL;
                free(config);
                errno = saved_errno;
                return -1;
        }
        return 0;
}
