#ifndef TRAFFIC_GEN_HTTP_CLIENT_H
#define TRAFFIC_GEN_HTTP_CLIENT_H

/**
 * @file http_client.h
 * @brief HTTP/1.x client protocol configuration and plugin exports.
 *
 * The HTTP plugin builds one request and parses one response for each
 * transaction.  It deliberately owns no sockets; @ref tg_flow supplies bytes
 * to the plugin and interprets its completion result.
 */

#include "../proto.h"

#include <stdbool.h>

/** @brief Capacity, including NUL, of an HTTP method token. */
#define TG_HTTP_METHOD_CAP 16U
/** @brief Capacity, including NUL, of an HTTP request path. */
#define TG_HTTP_PATH_CAP 768U
/** @brief Capacity, including NUL, of an HTTP Host header value. */
#define TG_HTTP_HOST_CAP 256U

/**
 * @brief Owning immutable request parameters for an HTTP traffic class.
 *
 * The protocol configuration owns method and path storage so a compiled plan
 * can clone it without retaining pointers into another plan or the JSON input
 * buffer.
 */
struct tg_http_config {
        char method[TG_HTTP_METHOD_CAP];
        char path[TG_HTTP_PATH_CAP];
        char host[TG_HTTP_HOST_CAP];
        bool connection_close;
        bool accept_any_status; /**< Business layer performs status checks. */
        /* Opt-in response retention; headers and decoded body are bounded separately. */
        bool capture_headers;
        bool capture_json;
};

/** @brief HTTP protocol operations used by scenario-compiled HTTP classes. */
extern const struct tg_proto_ops tg_http_proto_ops;

/**
 * @brief Serializes validated dynamic request fields with plugin-owned framing.
 * @param headers Validated custom fields, each terminated with CRLF.
 * @param body NUL-terminated request body; length is measured in bytes.
 * @param buffer Caller-owned output storage.
 * @param cap Output capacity, including the temporary string terminator.
 * @param length Receives the serialized wire length on success.
 * @return 0 on success; -1 when arguments or output capacity are invalid.
 *
 * The business layer rejects control headers and invalid field bytes before
 * calling this helper. The plugin generates Content-Length and Connection.
 */
int tg_http_build_parts(const struct tg_http_config *config,
                        const char *headers, const char *body, uint8_t *buffer,
                        size_t cap, size_t *length);

/** @brief Default HTTP GET configuration with explicit connection close. */
extern const struct tg_http_config tg_http_bootstrap_config;

#endif /* TRAFFIC_GEN_HTTP_CLIENT_H */
