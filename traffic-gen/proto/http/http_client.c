/**
 * @file http_client.c
 * @brief Implements the byte-only HTTP/1.x short-connection protocol plugin.
 *
 * The plugin serializes one HTTP/1.0 request and uses llhttp to validate one
 * HTTP/1.0 or HTTP/1.1 2xx response.  Socket ownership, I/O retries, and
 * flow reclamation remain outside this module in the transport layer.
 */

#include "http_client.h"

#include "../../../third_party/llhttp/include/llhttp.h"
#include "../../core/txn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/** @brief Conservative GET configuration for legacy bootstrap callers. */
const struct tg_http_config tg_http_bootstrap_config = {
    .method = "GET",
    .path = "/",
    .connection_close = true,
};

/**
 * @brief Per-transaction llhttp parser context owned through @c proto_ctx.
 */
struct tg_http_parser {
        llhttp_t parser;
        bool message_complete;
        uint64_t body_received;
};

/** @brief Retrieves a transaction's plugin-private HTTP parser context. */
static struct tg_http_parser *tg_http_parser(struct tg_txn *txn) {
        return txn == NULL ? NULL : txn->proto_ctx;
}

/** @brief Retrieves the context attached to an llhttp parser callback. */
static struct tg_http_parser *tg_http_context(llhttp_t *parser) {
        return parser == NULL ? NULL : parser->data;
}

/**
 * @brief Serializes one connection-close HTTP/1.0 request into caller storage.
 */
static int tg_http_build_request(const void *class_config, uint8_t *buffer,
                                 size_t buffer_cap, size_t *request_len_out) {
        const struct tg_http_config *config = class_config;
        const char *method;
        const char *path;
        const char *connection;
        int written;

        if (buffer == NULL || request_len_out == NULL || buffer_cap == 0) {
                errno = EINVAL;
                return -1;
        }

        method =
            config != NULL && config->method != NULL ? config->method : "GET";
        path = config != NULL && config->path != NULL ? config->path : "/";
        connection =
            config == NULL || config->connection_close ? "close" : "keep-alive";

        written = snprintf((char *)buffer, buffer_cap,
                           "%s %s HTTP/1.0\r\nConnection: %s\r\n\r\n", method,
                           path, connection);
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

/** @brief Accepts only HTTP/1.x responses with a successful 2xx status. */
static int tg_http_on_headers_complete(llhttp_t *parser) {
        unsigned int major = llhttp_get_http_major(parser);
        unsigned int minor = llhttp_get_http_minor(parser);
        int status_code = llhttp_get_status_code(parser);

        if (major != 1U || (minor != 0U && minor != 1U)) {
                llhttp_set_error_reason(parser,
                                        "only HTTP/1.0 and HTTP/1.1 supported");
                return -1;
        }
        if (status_code < 200 || status_code >= 300) {
                llhttp_set_error_reason(parser,
                                        "HTTP response status is not 2xx");
                return -1;
        }

        return 0;
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

        context->body_received += len;
        return 0;
}

/** @brief Marks the single expected HTTP response as complete. */
static int tg_http_on_message_complete(llhttp_t *parser) {
        struct tg_http_parser *context = tg_http_context(parser);

        if (context == NULL)
                return -1;

        context->message_complete = true;
        return 0;
}

/** @brief Callback table that constrains llhttp to this client's semantics. */
static const llhttp_settings_t tg_http_llhttp_settings = {
    .on_message_begin = tg_http_on_message_begin,
    .on_headers_complete = tg_http_on_headers_complete,
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

        llhttp_init(&context->parser, HTTP_RESPONSE, &tg_http_llhttp_settings);
        context->parser.data = context;
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

        return llhttp_finish(&context->parser) == HPE_OK &&
                       context->message_complete
                   ? TG_PROTO_COMPLETE
                   : TG_PROTO_FAILED;
}

/** @brief Releases the llhttp context held by a resetting transaction. */
static void tg_http_reset(struct tg_txn *txn) {
        if (txn == NULL)
                return;
        free(txn->proto_ctx);
        txn->proto_ctx = NULL;
}

/** @brief HTTP implementation of the generic @ref tg_proto_ops contract. */
const struct tg_proto_ops tg_http_proto_ops = {
    .name = "http",
    .init = tg_http_init,
    .build_request = tg_http_build_request,
    .on_tx_accepted = tg_http_on_tx_accepted,
    .on_rx = tg_http_on_rx,
    .on_eof = tg_http_on_eof,
    .reset = tg_http_reset,
};