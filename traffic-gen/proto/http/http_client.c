#include "http_client.h"

#include "../../core/txn.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

const struct tg_http_config tg_http_bootstrap_config = {
    .method = "GET",
    .path = "/",
    .connection_close = true,
};

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

static void tg_http_on_tx_accepted(struct tg_txn *txn,
                                   __attribute__((unused)) size_t bytes) {
        (void)txn;
}

static enum tg_proto_result
tg_http_on_rx(__attribute__((unused)) struct tg_txn *txn,
              __attribute__((unused)) const uint8_t *data,
              __attribute__((unused)) size_t len) {
        /*
         * This bootstrap adapter deliberately accepts arbitrary response bytes
         * so the existing TCP echo peer remains a valid transport regression
         * target.  llhttp-based HTTP response validation comes later.
         */
        return TG_PROTO_MORE;
}

static enum tg_proto_result
tg_http_on_eof(__attribute__((unused)) struct tg_txn *txn) {
        return TG_PROTO_COMPLETE;
}

static void tg_http_reset(__attribute__((unused)) struct tg_txn *txn) {}

const struct tg_proto_ops tg_http_proto_ops = {
    .name = "http",
    .build_request = tg_http_build_request,
    .on_tx_accepted = tg_http_on_tx_accepted,
    .on_rx = tg_http_on_rx,
    .on_eof = tg_http_on_eof,
    .reset = tg_http_reset,
};
