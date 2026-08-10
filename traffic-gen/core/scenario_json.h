#ifndef TRAFFIC_GEN_SCENARIO_JSON_H
#define TRAFFIC_GEN_SCENARIO_JSON_H

/**
 * @file scenario_json.h
 * @brief Small, schema-agnostic helpers for validated scenario JSON.
 *
 * These helpers operate on a tokenized, NUL-terminated JSMN document.  They
 * deliberately do not know about traffic classes or protocol configuration;
 * protocol-specific compilers can share the same validation behavior without
 * depending on scenario.c's private functions.
 */

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../third_party/jsmn/jsmn.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief A non-owning view over one tokenized scenario document.
 *
 * The document text and token array must outlive every operation using the
 * view.  Scenario loading keeps both alive until all protocol configurations
 * and request templates have been compiled.
 */
struct tg_json_doc {
        const char *text;
        const jsmntok_t *tokens;
        int token_count;
};

/** Compare a JSON token with an unescaped literal. */
bool tg_json_token_equal(const struct tg_json_doc *doc, const jsmntok_t *token,
                         const char *literal);

/**
 * Copy a non-empty, unescaped JSON string into fixed caller storage.
 *
 * @return 0 on success; -1 with @c errno set on invalid input or overflow.
 */
int tg_json_copy_string(char *out, size_t out_cap,
                        const struct tg_json_doc *doc, const jsmntok_t *token);

/**
 * Parse an unsigned decimal JSON primitive within inclusive bounds.
 *
 * @return 0 on success; -1 with @c errno set on invalid input or range error.
 */
int tg_json_parse_u32(const struct tg_json_doc *doc, const jsmntok_t *token,
                      uint32_t minimum, uint32_t maximum, uint32_t *value_out);

/** Parse only the JSON @c true and @c false primitive values. */
int tg_json_parse_bool(const struct tg_json_doc *doc, const jsmntok_t *token,
                       bool *value_out);

/** Return the value token index of a direct object member, or -1 if absent. */
int tg_json_object_value(const struct tg_json_doc *doc, int object_index,
                         const char *key);

/**
 * Verify that every direct key in an object belongs to an allowed-key list.
 *
 * Duplicate keys retain the existing scenario parser behavior: the first
 * matching value is returned by tg_json_object_value().
 */
bool tg_json_object_has_only(const struct tg_json_doc *doc, int object_index,
                             const char *const allowed[], size_t allowed_count);

#endif /* TRAFFIC_GEN_SCENARIO_JSON_H */
