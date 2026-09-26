#ifndef TG_VALUE_H
#define TG_VALUE_H

/**
 * @file value.h
 * @brief Bounded JSON documents and typed scalar values.
 *
 * Documents own source text and parser tokens. Values copy decoded scalar text
 * so extracted context survives protocol reset. These helpers are used at startup
 * and for explicitly requested response extraction, not legacy request parsing.
 */

#include "scenario_json.h"
#define TG_VALUE_CAP 1025U
enum tg_value_kind {
        TG_VALUE_NULL,
        TG_VALUE_STRING,
        TG_VALUE_NUMBER,
        TG_VALUE_BOOL
};
struct tg_value {
        enum tg_value_kind kind;
        /* Decoded UTF-8 or primitive JSON text; 1 KiB payload plus terminator. */
        char text[TG_VALUE_CAP];
};
struct tg_document {
        /* Borrowed view into the text/tokens owned by this same document. */
        struct tg_json_doc view;
        char *text;
        jsmntok_t *tokens;
};

/**
 * @brief Copies and strictly parses a length-delimited JSON document.
 *
 * @param out Receives owned text and tokens; free with tg_document_free.
 * @return 0 on success; -1 with partial allocations released.
 */
int tg_document_parse(struct tg_document *out, const char *text, size_t length);

/**
 * @brief Releases owned text/tokens and clears the document.
 *
 * All previously borrowed views and token indices become invalid.
 */
void tg_document_free(struct tg_document *doc);

/**
 * @brief Copies a scalar token into independent typed storage.
 *
 * Objects, arrays, missing tokens, and oversized values are rejected.
 * @return 0 on success; -1 when the token cannot be represented.
 */
int tg_value_read(const struct tg_json_doc *doc, int token,
                  struct tg_value *out);

/**
 * @brief Decodes JSON escapes and validates UTF-8 without truncation.
 *
 * @param cap Output capacity including the trailing NUL.
 * @return 0 on success; -1 for invalid encoding, embedded NUL, or overflow.
 */
int tg_string_read(const struct tg_json_doc *doc, int token, char *out,
                   size_t cap);

/**
 * @brief Skips one valid token subtree.
 *
 * @return The next sibling position, possibly equal to token_count.
 */
int tg_json_next(const struct tg_json_doc *doc, int token);

/**
 * @brief Resolves a JSON Pointer to an object member or array item.
 *
 * Supports ~0/~1 escaping; duplicate selected keys and leading-zero array
 * indices fail. An empty pointer selects the root, which need not be scalar.
 * @return A token index, or -1 for an invalid or unresolved path.
 */
int tg_json_pointer(const struct tg_json_doc *doc, const char *pointer);

/**
 * @brief Compares matching scalar types without implicit coercion.
 *
 * Numbers compare numerically and strings by byte order; bool/null only allow
 * equality or inequality.
 * @return 0 with the result in out; -1 for type or operator mismatch.
 */
int tg_value_compare(const struct tg_value *a, const char *op,
                     const struct tg_value *b, bool *out);
#endif
