/**
 * @file scenario_json.c
 * @brief Implements schema-agnostic helpers for validated scenario JSON.
 */

#include "scenario_json.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

/** @copydoc tg_json_token_equal */
bool tg_json_token_equal(const struct tg_json_doc *doc, const jsmntok_t *token,
                         const char *literal) {
        size_t length;

        if (doc == NULL || doc->text == NULL || token == NULL ||
            literal == NULL || token->start < 0 || token->end < token->start)
                return false;
        length = strlen(literal);
        return token->end - token->start == (int)length &&
               memcmp(doc->text + token->start, literal, length) == 0;
}

/** @copydoc tg_json_copy_string */
int tg_json_copy_string(char *out, size_t out_cap,
                        const struct tg_json_doc *doc, const jsmntok_t *token) {
        size_t length;

        if (out == NULL || out_cap == 0 || doc == NULL || doc->text == NULL ||
            token == NULL || token->type != JSMN_STRING || token->start < 0 ||
            token->end < token->start) {
                errno = EINVAL;
                return -1;
        }
        length = (size_t)(token->end - token->start);
        if (length == 0 || length >= out_cap ||
            memchr(doc->text + token->start, '\\', length) != NULL) {
                errno = EINVAL;
                return -1;
        }
        memcpy(out, doc->text + token->start, length);
        out[length] = '\0';
        return 0;
}

/** @copydoc tg_json_parse_u32 */
int tg_json_parse_u32(const struct tg_json_doc *doc, const jsmntok_t *token,
                      uint32_t minimum, uint32_t maximum, uint32_t *value_out) {
        uint64_t value = 0;

        if (doc == NULL || doc->text == NULL || token == NULL ||
            value_out == NULL || token->type != JSMN_PRIMITIVE ||
            token->start < 0 || token->start >= token->end) {
                errno = EINVAL;
                return -1;
        }
        for (int i = token->start; i < token->end; i++) {
                unsigned int digit;

                if (doc->text[i] < '0' || doc->text[i] > '9') {
                        errno = EINVAL;
                        return -1;
                }
                digit = (unsigned int)(doc->text[i] - '0');
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

/** @copydoc tg_json_parse_bool */
int tg_json_parse_bool(const struct tg_json_doc *doc, const jsmntok_t *token,
                       bool *value_out) {
        if (doc == NULL || token == NULL || value_out == NULL ||
            token->type != JSMN_PRIMITIVE) {
                errno = EINVAL;
                return -1;
        }
        if (tg_json_token_equal(doc, token, "true")) {
                *value_out = true;
                return 0;
        }
        if (tg_json_token_equal(doc, token, "false")) {
                *value_out = false;
                return 0;
        }
        errno = EINVAL;
        return -1;
}

/** @copydoc tg_json_object_value */
int tg_json_object_value(const struct tg_json_doc *doc, int object_index,
                         const char *key) {
        if (doc == NULL || doc->tokens == NULL || doc->text == NULL ||
            key == NULL || object_index < 0 || object_index >= doc->token_count)
                return -1;

        for (int i = object_index + 1; i < doc->token_count; i++) {
                if (doc->tokens[i].parent != object_index)
                        continue;
                if (doc->tokens[i].type != JSMN_STRING)
                        continue;
                if (!tg_json_token_equal(doc, &doc->tokens[i], key))
                        continue;
                if (i + 1 >= doc->token_count || doc->tokens[i + 1].parent != i)
                        return -1;
                return i + 1;
        }
        return -1;
}

/** @copydoc tg_json_object_has_only */
bool tg_json_object_has_only(const struct tg_json_doc *doc, int object_index,
                             const char *const allowed[],
                             size_t allowed_count) {
        if (doc == NULL || doc->tokens == NULL || doc->text == NULL ||
            allowed == NULL || object_index < 0 ||
            object_index >= doc->token_count)
                return false;

        for (int i = object_index + 1; i < doc->token_count; i++) {
                bool found = false;

                if (doc->tokens[i].parent != object_index)
                        continue;
                if (doc->tokens[i].type != JSMN_STRING)
                        return false;
                for (size_t j = 0; j < allowed_count; j++) {
                        if (tg_json_token_equal(doc, &doc->tokens[i],
                                                allowed[j])) {
                                found = true;
                                break;
                        }
                }
                if (!found)
                        return false;
        }
        return true;
}
