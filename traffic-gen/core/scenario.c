/**
 * @file scenario.c
 * @brief Compiles a strictly validated JSON scenario into a runtime plan.
 *
 * This is startup-only control-plane code.  It rejects unknown schema fields,
 * unsupported escaped strings, and out-of-range values so the owner-local hot
 * path can consume fixed-size, pointer-stable plan data without JSON parsing.
 */

#include "scenario.h"

#include "../../third_party/jsmn/jsmn.h"

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

/** @brief Compares a JSON token with an unescaped schema literal. */
static bool tg_token_equal(const char *json, const jsmntok_t *token,
                           const char *literal) {
        size_t length;

        if (token == NULL || literal == NULL)
                return false;
        length = strlen(literal);
        return token->end - token->start == (int)length &&
               memcmp(json + token->start, literal, length) == 0;
}

/**
 * @brief Copies an unescaped nonempty JSON string into fixed plan storage.
 *
 * Escape sequences are rejected intentionally until an explicit JSON unescape
 * implementation exists; passing raw escape bytes into a request would be
 * incorrect.
 */
static int tg_copy_string(char *out, size_t out_cap, const char *json,
                          const jsmntok_t *token) {
        size_t length;

        if (out == NULL || out_cap == 0 || json == NULL || token == NULL ||
            token->type != JSMN_STRING || token->start < 0 ||
            token->end < token->start) {
                errno = EINVAL;
                return -1;
        }
        length = (size_t)(token->end - token->start);
        if (length == 0 || length >= out_cap ||
            memchr(json + token->start, '\\', length) != NULL) {
                errno = EINVAL;
                return -1;
        }
        memcpy(out, json + token->start, length);
        out[length] = '\0';
        return 0;
}

/** @brief Parses an unsigned decimal primitive within inclusive schema bounds.
 */
static int tg_parse_u32(const char *json, const jsmntok_t *token,
                        uint32_t minimum, uint32_t maximum,
                        uint32_t *value_out) {
        uint64_t value = 0;

        if (json == NULL || token == NULL || value_out == NULL ||
            token->type != JSMN_PRIMITIVE || token->start >= token->end) {
                errno = EINVAL;
                return -1;
        }
        for (int i = token->start; i < token->end; i++) {
                unsigned int digit;

                if (json[i] < '0' || json[i] > '9') {
                        errno = EINVAL;
                        return -1;
                }
                digit = (unsigned int)(json[i] - '0');
                if (value > (UINT32_MAX - digit) / 10U) {
                        errno = ERANGE;
                        return -1;
                }
                value = value * 10U + digit;
        }
        if (value < minimum || value > maximum) {
                errno = ERANGE;
                return -1;
        }
        *value_out = (uint32_t)value;
        return 0;
}

/** @brief Parses only the JSON @c true or @c false primitive values. */
static int tg_parse_bool(const char *json, const jsmntok_t *token,
                         bool *value_out) {
        if (json == NULL || token == NULL || value_out == NULL ||
            token->type != JSMN_PRIMITIVE) {
                errno = EINVAL;
                return -1;
        }
        if (tg_token_equal(json, token, "true")) {
                *value_out = true;
                return 0;
        }
        if (tg_token_equal(json, token, "false")) {
                *value_out = false;
                return 0;
        }
        errno = EINVAL;
        return -1;
}

/** @brief Returns the token index of a direct object member's value. */
static int tg_object_value(const jsmntok_t *tokens, int token_count,
                           int object_index, const char *json,
                           const char *key) {
        for (int i = object_index + 1; i < token_count; i++) {
                if (tokens[i].parent != object_index)
                        continue;
                if (tokens[i].type != JSMN_STRING)
                        continue;
                if (!tg_token_equal(json, &tokens[i], key))
                        continue;
                if (i + 1 >= token_count || tokens[i + 1].parent != i)
                        return -1;
                return i + 1;
        }
        return -1;
}

/** @brief Verifies that every direct object key occurs in an allowed-key list.
 */
static bool tg_object_has_only(const jsmntok_t *tokens, int token_count,
                               int object_index, const char *json,
                               const char *const allowed[],
                               size_t allowed_count) {
        for (int i = object_index + 1; i < token_count; i++) {
                bool found = false;

                if (tokens[i].parent != object_index)
                        continue;
                if (tokens[i].type != JSMN_STRING)
                        return false;
                for (size_t j = 0; j < allowed_count; j++) {
                        if (tg_token_equal(json, &tokens[i], allowed[j])) {
                                found = true;
                                break;
                        }
                }
                if (!found)
                        return false;
        }
        return true;
}

/**
 * @brief Compiles one HTTP object and binds its stable class-owned strings.
 */
static int tg_parse_http(const char *json, const jsmntok_t *tokens,
                         int token_count, int object_index,
                         struct tg_class_plan *class_plan) {
        static const char *const allowed[] = {"method", "path", "keepalive"};
        int method_index;
        int path_index;
        int keepalive_index;
        bool keepalive = false;

        if (tokens[object_index].type != JSMN_OBJECT ||
            !tg_object_has_only(tokens, token_count, object_index, json,
                                allowed,
                                sizeof(allowed) / sizeof(allowed[0]))) {
                errno = EINVAL;
                return -1;
        }
        method_index =
            tg_object_value(tokens, token_count, object_index, json, "method");
        path_index =
            tg_object_value(tokens, token_count, object_index, json, "path");
        keepalive_index = tg_object_value(tokens, token_count, object_index,
                                          json, "keepalive");
        if (method_index < 0 || path_index < 0 ||
            tg_copy_string(class_plan->http_method,
                           sizeof(class_plan->http_method), json,
                           &tokens[method_index]) != 0 ||
            tg_copy_string(class_plan->http_path, sizeof(class_plan->http_path),
                           json, &tokens[path_index]) != 0 ||
            class_plan->http_path[0] != '/') {
                errno = EINVAL;
                return -1;
        }
        if (keepalive_index >= 0 &&
            tg_parse_bool(json, &tokens[keepalive_index], &keepalive) != 0)
                return -1;

        class_plan->http_config.method = class_plan->http_method;
        class_plan->http_config.path = class_plan->http_path;
        class_plan->http_config.connection_close = !keepalive;
        class_plan->proto = &tg_http_proto_ops;
        return 0;
}

/** @brief Validates and compiles an IPv4 TCP peer object. */
static int tg_parse_peer(const char *json, const jsmntok_t *tokens,
                         int token_count, int object_index,
                         struct tg_class_plan *class_plan) {
        static const char *const allowed[] = {"ip", "port"};
        char ip[INET_ADDRSTRLEN];
        int ip_index;
        int port_index;
        uint32_t port;

        if (tokens[object_index].type != JSMN_OBJECT ||
            !tg_object_has_only(tokens, token_count, object_index, json,
                                allowed,
                                sizeof(allowed) / sizeof(allowed[0]))) {
                errno = EINVAL;
                return -1;
        }
        ip_index =
            tg_object_value(tokens, token_count, object_index, json, "ip");
        port_index =
            tg_object_value(tokens, token_count, object_index, json, "port");
        if (ip_index < 0 || port_index < 0 ||
            tg_copy_string(ip, sizeof(ip), json, &tokens[ip_index]) != 0 ||
            tg_parse_u32(json, &tokens[port_index], 1, UINT16_MAX, &port) !=
                0) {
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

/**
 * @brief Compiles one complete TCP/HTTP traffic-class declaration.
 */
static int tg_parse_class(const char *json, const jsmntok_t *tokens,
                          int token_count, int object_index,
                          struct tg_class_plan *class_plan) {
        static const char *const allowed[] = {"name", "weight", "transport",
                                              "peer", "http"};
        int name_index;
        int weight_index;
        int transport_index;
        int peer_index;
        int http_index;

        if (tokens[object_index].type != JSMN_OBJECT ||
            !tg_object_has_only(tokens, token_count, object_index, json,
                                allowed,
                                sizeof(allowed) / sizeof(allowed[0]))) {
                errno = EINVAL;
                return -1;
        }
        name_index =
            tg_object_value(tokens, token_count, object_index, json, "name");
        weight_index =
            tg_object_value(tokens, token_count, object_index, json, "weight");
        transport_index = tg_object_value(tokens, token_count, object_index,
                                          json, "transport");
        peer_index =
            tg_object_value(tokens, token_count, object_index, json, "peer");
        http_index =
            tg_object_value(tokens, token_count, object_index, json, "http");
        if (name_index < 0 || weight_index < 0 || transport_index < 0 ||
            peer_index < 0 || http_index < 0 ||
            !tg_token_equal(json, &tokens[transport_index], "tcp") ||
            tg_copy_string(class_plan->name, sizeof(class_plan->name), json,
                           &tokens[name_index]) != 0 ||
            tg_parse_u32(json, &tokens[weight_index], 1, UINT32_MAX,
                         &class_plan->weight) != 0 ||
            tg_parse_peer(json, tokens, token_count, peer_index, class_plan) !=
                0 ||
            tg_parse_http(json, tokens, token_count, http_index, class_plan) !=
                0) {
                errno = EINVAL;
                return -1;
        }
        class_plan->transport = TG_TRANSPORT_TCP;
        return 0;
}

/**
 * @brief Compiles the class array and validates its aggregate weight.
 */
static int tg_parse_classes(const char *json, const jsmntok_t *tokens,
                            int token_count, int array_index,
                            struct tg_plan *plan) {
        uint64_t total_weight = 0;

        if (tokens[array_index].type != JSMN_ARRAY ||
            tokens[array_index].size == 0 ||
            tokens[array_index].size > (int)TG_PLAN_MAX_CLASSES) {
                errno = EINVAL;
                return -1;
        }
        for (int i = array_index + 1; i < token_count; i++) {
                if (tokens[i].parent != array_index)
                        continue;
                if (plan->class_count >= TG_PLAN_MAX_CLASSES ||
                    tg_parse_class(json, tokens, token_count, i,
                                   &plan->classes[plan->class_count]) != 0)
                        return -1;
                total_weight += plan->classes[plan->class_count].weight;
                if (total_weight > UINT32_MAX) {
                        errno = ERANGE;
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
        if (token_count <= 0 || tokens[0].type != JSMN_OBJECT ||
            !tg_object_has_only(tokens, token_count, 0, json, allowed,
                                sizeof(allowed) / sizeof(allowed[0]))) {
                errno = EINVAL;
                goto out;
        }
        name_index = tg_object_value(tokens, token_count, 0, json, "name");
        duration_index =
            tg_object_value(tokens, token_count, 0, json, "duration_sec");
        concurrency_index =
            tg_object_value(tokens, token_count, 0, json, "max_concurrency");
        cps_index = tg_object_value(tokens, token_count, 0, json, "target_cps");
        report_index = tg_object_value(tokens, token_count, 0, json,
                                       "report_interval_sec");
        classes_index =
            tg_object_value(tokens, token_count, 0, json, "classes");
        if (name_index < 0 || duration_index < 0 || concurrency_index < 0 ||
            cps_index < 0 || classes_index < 0 ||
            tg_copy_string(plan->name, sizeof(plan->name), json,
                           &tokens[name_index]) != 0 ||
            tg_parse_u32(json, &tokens[duration_index], 1,
                         TG_PLAN_MAX_DURATION_SEC, &plan->duration_sec) != 0 ||
            tg_parse_u32(json, &tokens[concurrency_index], 1,
                         TG_PLAN_MAX_CONCURRENCY,
                         &plan->max_concurrency) != 0 ||
            tg_parse_u32(json, &tokens[cps_index], 1, TG_PLAN_MAX_CPS,
                         &plan->target_cps) != 0 ||
            (report_index >= 0 &&
             tg_parse_u32(json, &tokens[report_index], 1,
                          TG_PLAN_MAX_REPORT_INTERVAL_SEC,
                          &plan->report_interval_sec) != 0) ||
            tg_parse_classes(json, tokens, token_count, classes_index, plan) !=
                0)
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

/** @copydoc tg_plan_fini */
void tg_plan_fini(struct tg_plan *plan) {
        if (plan != NULL)
                memset(plan, 0, sizeof(*plan));
}
