/**
 * @file http_client.c
 * @brief Implements the byte-only HTTP/1.x client protocol plugin.
 *
 * The plugin serializes one HTTP/1.1 request and uses llhttp to validate one
 * HTTP/1.0 or HTTP/1.1 2xx response.  A completed response reports whether
 * the transport may reuse its TCP connection.
 */

#include "http_client.h"

#include "../../../third_party/llhttp/include/llhttp.h"
#include "../../core/txn.h"
#include "../../core/value.h"
#include <string.h>
#include <strings.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/** @brief Conservative GET configuration for legacy bootstrap callers. */
const struct tg_http_config tg_http_bootstrap_config = {
    .method = "GET",
    .path = "/",
    .host = "localhost",
    .connection_close = true,
};

/**
 * @brief Per-transaction llhttp parser context owned through @c proto_ctx.
 */
struct tg_http_parser {
        llhttp_t parser;
        struct tg_txn *txn;
        bool message_complete;
        bool connection_reusable;
        uint64_t body_received;
        unsigned status;
        /* Allocated only when requested. Headers retain ordered name/value
         * pairs; body contains bytes after HTTP chunk framing is removed. */
        char *headers, *body;
        size_t headers_len, body_len;
        bool header_value;
        /* Parsed lazily on first JSON export and reused for later Pointer lookups. */
        struct tg_document json;
};

/** @brief Retrieves a transaction's plugin-private HTTP parser context. */
static struct tg_http_parser *tg_http_parser(struct tg_txn *txn) {
        return txn == NULL ? NULL : txn->proto_ctx;
}

/** @brief Retrieves the context attached to an llhttp parser callback. */
static struct tg_http_parser *tg_http_context(llhttp_t *parser) {
        return parser == NULL ? NULL : parser->data;
}

/** @brief Clones owning HTTP class configuration for a scheduling shard. */
static int tg_http_config_clone(const void *source, void **destination) {
        const struct tg_http_config *source_config = source;
        struct tg_http_config *copy;

        if (source_config == NULL || destination == NULL) {
                errno = EINVAL;
                return -1;
        }

        copy = malloc(sizeof(*copy));
        if (copy == NULL)
                return -1;
        *copy = *source_config;
        *destination = copy;
        return 0;
}

/** @brief Releases one heap-owned HTTP class configuration. */
static void tg_http_config_free(void *config) { free(config); }

/** @brief Serializes one HTTP/1.1 request into caller storage. */
static int tg_http_build_request(const void *class_config, uint8_t *buffer,
                                 size_t buffer_cap, size_t *request_len_out) {
        const struct tg_http_config *config = class_config;
        const char *method;
        const char *path;
        const char *host;
        const char *connection;
        int written;

        if (buffer == NULL || request_len_out == NULL || buffer_cap == 0) {
                errno = EINVAL;
                return -1;
        }

        method = config != NULL && config->method[0] != '\0' ? config->method
                                                             : "GET";
        path = config != NULL && config->path[0] != '\0' ? config->path : "/";
        host = config != NULL && config->host[0] != '\0' ? config->host
                                                         : "localhost";
        connection =
            config == NULL || config->connection_close ? "close" : "keep-alive";

        written = snprintf((char *)buffer, buffer_cap,
                           "%s %s HTTP/1.1\r\nHost: %s\r\n"
                           "Connection: %s\r\n\r\n",
                           method, path, host, connection);
        if (written < 0 || (size_t)written >= buffer_cap) {
                errno = EMSGSIZE;
                return -1;
        }

        *request_len_out = (size_t)written;
        return 0;
}

/** @brief Rejects a second response message on the same short connection. */
static int tg_http_on_message_begin(llhttp_t *parser) {
        struct tg_http_parser *context = tg_http_context(parser);

        if (context == NULL)
                return -1;
        if (context->message_complete) {
                llhttp_set_error_reason(parser,
                                        "multiple HTTP responses unsupported");
                return -1;
        }

        return 0;
}

/** @brief Validates framing and applies legacy status policy to final responses. */
static int tg_http_on_headers_complete(llhttp_t *parser) {
        unsigned int major = llhttp_get_http_major(parser);
        unsigned int minor = llhttp_get_http_minor(parser);
        int status_code = llhttp_get_status_code(parser);

        if (major != 1U || (minor != 0U && minor != 1U)) {
                llhttp_set_error_reason(parser,
                                        "only HTTP/1.0 and HTTP/1.1 supported");
                return -1;
        }
        struct tg_http_parser *context = tg_http_context(parser);
        const struct tg_http_config *config = context->txn->class_config;
        context->status = (unsigned)status_code;
        if (status_code == 101) {
                llhttp_set_error_reason(parser, "protocol upgrades unsupported");
                return -1;
        }
        if (status_code >= 100 && status_code < 200)
                return 0;
        if (!(config && config->accept_any_status) &&
            (status_code < 200 || status_code >= 300)) {
                llhttp_set_error_reason(parser,
                                        "HTTP response status is not 2xx");
                return -1;
        }

        /* A HEAD response advertises the GET body length but carries no body. */
        return config && !strcmp(config->method, "HEAD") ? 1 : 0;
}

/** @brief Counts parsed response-body bytes with overflow protection. */
static int tg_http_on_body(llhttp_t *parser, const char *data, size_t len) {
        struct tg_http_parser *context = tg_http_context(parser);

        if (context == NULL || (data == NULL && len != 0))
                return -1;
        if (len > UINT64_MAX - context->body_received) {
                llhttp_set_error_reason(parser,
                                        "HTTP body byte counter overflow");
                return -1;
        }

        const struct tg_http_config *config = context->txn->class_config;
        if (config && config->capture_json) {
                if (len > 65536U - context->body_len)
                        return -1;
                if (!context->body && !(context->body = malloc(65537U)))
                        return -1;
                memcpy(context->body + context->body_len, data, len);
                context->body_len += len;
                context->body[context->body_len] = 0;
        }
        context->body_received += len;
        return 0;
}

/** @brief Marks the single expected HTTP response as complete. */
static int tg_http_on_message_complete(llhttp_t *parser) {
        struct tg_http_parser *context = tg_http_context(parser);
        const struct tg_http_config *config;

        if (context == NULL)
                return -1;

        if (context->status < 200) {
                /* Keep parsing after an informational message. Its headers
                 * do not belong to the final response's extraction namespace. */
                context->headers_len = 0;
                if (context->headers)
                        context->headers[0] = 0;
                return 0;
        }
        context->message_complete = true;
        config = context->txn == NULL ? NULL : context->txn->class_config;
        context->connection_reusable =
            config != NULL && !config->connection_close &&
            llhttp_should_keep_alive(parser);
        if (context->txn != NULL)
                context->txn->connection_reusable =
                    context->connection_reusable;
        return 0;
}

/* Header callbacks can split at any byte. Store NUL-separated name/value
 * pairs only for workflows that request headers; repeated fields stay distinct.
 */
static int tg_http_capture(llhttp_t *parser, const char *data, size_t len,
                           bool value) {
        struct tg_http_parser *c = tg_http_context(parser);
        const struct tg_http_config *config = c->txn->class_config;
        if (!config || !config->capture_headers)
                return 0;
        if (!c->headers && !(c->headers = malloc(65536U)))
                return -1;
        (void)value;
        if (len > 65535U - c->headers_len)
                return -1;
        memcpy(c->headers + c->headers_len, data, len);
        c->headers_len += len;
        c->headers[c->headers_len] = 0;
        return 0;
}

/**
 * @brief Terminates a captured name or value at an llhttp field boundary.
 *
 * Data callbacks may split anywhere or be absent for an empty value. Explicit
 * completion callbacks preserve empty and repeated headers without guessing
 * boundaries from individual receive fragments.
 */
static int tg_http_header_end(llhttp_t *parser) {
        struct tg_http_parser *c = tg_http_context(parser);
        const struct tg_http_config *config = c->txn->class_config;
        if (!config || !config->capture_headers)
                return 0;
        if (!c->headers && !(c->headers = malloc(65536U)))
                return -1;
        if (c->headers_len >= 65535U)
                return -1;
        c->headers[c->headers_len++] = 0;
        c->headers[c->headers_len] = 0;
        return 0;
}
static int tg_http_header_name(llhttp_t *p, const char *s, size_t n) {
        return tg_http_capture(p, s, n, false);
}
static int tg_http_header_value(llhttp_t *p, const char *s, size_t n) {
        return tg_http_capture(p, s, n, true);
}

/**
 * @brief Copies a scalar from a complete response before protocol reset.
 *
 * Header matching ignores case and uses a zero-based duplicate index. JSON
 * parsing is lazy and cached, but the selected value must fit scalar storage.
 * @return 0 on success; -1 for incomplete, missing, invalid, or oversized data.
 */
static int tg_http_export(const struct tg_txn *txn, const char *source,
                          const char *path, unsigned index,
                          struct tg_value *out) {
        struct tg_http_parser *c = txn->proto_ctx;
        if (!c || !c->message_complete)
                return -1;
        if (!strcmp(source, "status")) {
                out->kind = TG_VALUE_NUMBER;
                snprintf(out->text, sizeof(out->text), "%u", c->status);
                return 0;
        }
        if (!strcmp(source, "header") && c->headers) {
                size_t offset = 0;
                while (offset < c->headers_len) {
                        const char *name = c->headers + offset;
                        offset += strlen(name) + 1;
                        if (offset > c->headers_len)
                                return -1;
                        const char *value = c->headers + offset;
                        offset += strlen(value) + 1;
                        if (!strcasecmp(name, path) && index-- == 0) {
                                if (strlen(value) >= sizeof(out->text))
                                        return -1;
                                out->kind = TG_VALUE_STRING;
                                strcpy(out->text, value);
                                return 0;
                        }
                }
        }
        if (!strcmp(source, "json") && c->body) {
                if (!c->json.text &&
                    tg_document_parse(&c->json, c->body, c->body_len))
                        return -1;
                return tg_value_read(&c->json.view,
                                     tg_json_pointer(&c->json.view, path), out);
        }
        return -1;
}

/** @copydoc tg_http_build_parts */
int tg_http_build_parts(const struct tg_http_config *config,
                        const char *headers, const char *body, uint8_t *buffer,
                        size_t cap, size_t *length) {
        int n = snprintf((char *)buffer, cap,
                         "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: "
                         "%s\r\n%sContent-Length: %zu\r\n\r\n%s",
                         config->method, config->path, config->host,
                         config->connection_close ? "close" : "keep-alive",
                         headers, strlen(body), body);
        if (n < 0 || (size_t)n >= cap) {
                errno = EMSGSIZE;
                return -1;
        }
        *length = (size_t)n;
        return 0;
}

/** @brief Callback table that constrains llhttp to this client's semantics. */
static const llhttp_settings_t tg_http_llhttp_settings = {
    .on_message_begin = tg_http_on_message_begin,
    .on_headers_complete = tg_http_on_headers_complete,
    .on_body = tg_http_on_body,
    .on_message_complete = tg_http_on_message_complete,
};
/* Extra field callbacks are only installed for Header capture. Legacy traffic
 * keeps the smaller callback table; body capture has its own opt-in flag. */
static const llhttp_settings_t tg_http_capture_settings = {
    .on_message_begin = tg_http_on_message_begin,
    .on_headers_complete = tg_http_on_headers_complete,
    .on_header_field = tg_http_header_name,
    .on_header_value = tg_http_header_value,
    .on_header_field_complete = tg_http_header_end,
    .on_header_value_complete = tg_http_header_end,
    .on_body = tg_http_on_body,
    .on_message_complete = tg_http_on_message_complete,
};

/** @brief Allocates and initializes an llhttp response parser for a
 * transaction. */
static int tg_http_init(struct tg_txn *txn) {
        struct tg_http_parser *context;

        if (txn == NULL) {
                errno = EINVAL;
                return -1;
        }

        context = calloc(1, sizeof(*context));
        if (context == NULL) {
                return -1;
        }

        const struct tg_http_config *config = txn->class_config;
        llhttp_init(&context->parser, HTTP_RESPONSE,
                    config && config->capture_headers
                        ? &tg_http_capture_settings
                        : &tg_http_llhttp_settings);
        context->parser.data = context;
        context->txn = txn;
        txn->proto_ctx = context;
        return 0;
}

/** @brief Accepts transport progress; HTTP has no transmit-side state today. */
static void tg_http_on_tx_accepted(__attribute__((unused)) struct tg_txn *txn,
                                   __attribute__((unused)) size_t bytes) {}

/** @brief Parses an arbitrary response chunk and reports message progress. */
static enum tg_proto_result tg_http_on_rx(struct tg_txn *txn,
                                          const uint8_t *data, size_t len) {
        struct tg_http_parser *context = tg_http_parser(txn);
        llhttp_errno_t error;

        if (context == NULL || (data == NULL && len != 0))
                return TG_PROTO_FAILED;

        if (context->message_complete)
                return len == 0 ? TG_PROTO_COMPLETE : TG_PROTO_FAILED;

        error = llhttp_execute(&context->parser, (const char *)data, len);
        if (error != HPE_OK)
                return TG_PROTO_FAILED;

        txn->connection_reusable = context->connection_reusable;
        return context->message_complete ? TG_PROTO_COMPLETE : TG_PROTO_MORE;
}

/**
 * @brief Completes an EOF-delimited response only when llhttp permits it.
 */
static enum tg_proto_result tg_http_on_eof(struct tg_txn *txn) {
        struct tg_http_parser *context = tg_http_parser(txn);

        if (context == NULL)
                return TG_PROTO_FAILED;
        if (context->message_complete)
                return TG_PROTO_COMPLETE;
        if (!llhttp_message_needs_eof(&context->parser))
                return TG_PROTO_FAILED;

        if (llhttp_finish(&context->parser) != HPE_OK ||
            !context->message_complete)
                return TG_PROTO_FAILED;
        context->connection_reusable = false;
        txn->connection_reusable = false;
        return TG_PROTO_COMPLETE;
}

/** @brief Releases the llhttp context held by a resetting transaction. */
static void tg_http_reset(struct tg_txn *txn) {
        if (txn == NULL)
                return;
        struct tg_http_parser *context = txn->proto_ctx;
        if (context) {
                /* The Flow callback has already copied any required business values. */
                free(context->headers);
                free(context->body);
                tg_document_free(&context->json);
        }
        free(txn->proto_ctx);
        txn->proto_ctx = NULL;
}

/** @brief HTTP implementation of the generic @ref tg_proto_ops contract. */
const struct tg_proto_ops tg_http_proto_ops = {
    .name = "http",
    .config_clone = tg_http_config_clone,
    .config_free = tg_http_config_free,
    .init = tg_http_init,
    .build_request = tg_http_build_request,
    .on_tx_accepted = tg_http_on_tx_accepted,
    .on_rx = tg_http_on_rx,
    .on_eof = tg_http_on_eof,
    .export_value = tg_http_export,
    .reset = tg_http_reset,
};
