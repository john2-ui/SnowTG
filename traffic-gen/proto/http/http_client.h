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

/**
 * @brief Immutable request parameters supplied by an HTTP traffic class.
 *
 * @p method and @p path must remain valid for the complete transaction.  A
 * compiled scenario stores them in its class plan, rather than in the JSON
 * input buffer, to satisfy that lifetime requirement.
 */
struct tg_http_config {
        const char *method;
        const char *path;
        bool connection_close;
};

/** @brief HTTP protocol operations used by scenario-compiled HTTP classes. */
extern const struct tg_proto_ops tg_http_proto_ops;

/** @brief Default close-delimited HTTP GET configuration for bootstrap use. */
extern const struct tg_http_config tg_http_bootstrap_config;

#endif /* TRAFFIC_GEN_HTTP_CLIENT_H */
