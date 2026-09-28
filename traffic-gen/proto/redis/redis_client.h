#ifndef TRAFFIC_GEN_REDIS_CLIENT_H
#define TRAFFIC_GEN_REDIS_CLIENT_H

#include "../proto.h"
#include <stdbool.h>

enum tg_redis_command { TG_REDIS_PING, TG_REDIS_GET, TG_REDIS_SET };

/** UTF-8 scenario arguments; the complete encoded request is capped at 1 KiB. */
struct tg_redis_config {
        enum tg_redis_command command;
        bool keepalive;
        char key[1025];
        char value[1025];
};

/** RESP2, one outstanding PING/GET/SET per connection; owns no sockets. */
extern const struct tg_proto_ops tg_redis_proto_ops;

#endif
