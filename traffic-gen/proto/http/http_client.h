#ifndef TRAFFIC_GEN_HTTP_CLIENT_H
#define TRAFFIC_GEN_HTTP_CLIENT_H

#include "../proto.h"

#include <stdbool.h>

struct tg_http_config {
        const char *method;
        const char *path;
        bool connection_close;
};

extern const struct tg_proto_ops tg_http_proto_ops;
extern const struct tg_http_config tg_http_bootstrap_config;

#endif /* TRAFFIC_GEN_HTTP_CLIENT_H */
