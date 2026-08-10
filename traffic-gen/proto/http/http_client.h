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
        bool connection_close;
};

/** @brief HTTP protocol operations used by scenario-compiled HTTP classes. */
extern const struct tg_proto_ops tg_http_proto_ops;

/** @brief Default close-delimited HTTP GET configuration for bootstrap use. */
extern const struct tg_http_config tg_http_bootstrap_config;

#endif /* TRAFFIC_GEN_HTTP_CLIENT_H */
