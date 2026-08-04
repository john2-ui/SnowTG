#ifndef TRAFFIC_GEN_FLOW_H
#define TRAFFIC_GEN_FLOW_H

/**
 * @file flow.h
 * @brief Owner-local TCP flow identity, transport state, and socket mapping.
 */

#include "txn.h"

#include "../../pro-stack/owner_io.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define TG_FLOW_RESPONSE_CAP 4096U

enum tg_flow_state {
        TG_FLOW_NEW = 0,
        TG_FLOW_CONNECTING,
        TG_FLOW_SENDING,
        TG_FLOW_RECEIVING,
        TG_FLOW_DONE,
        TG_FLOW_FAILED,
};

struct tg_flow;
struct tg_flow_pool;

struct tg_flow_map {
        struct tg_flow **by_socket_id;
        uint16_t owner_lcore;
};

struct tg_flow {
        struct nsock_handle handle;
        enum tg_flow_state state;
        struct tg_txn txn;

        bool mapped;
        bool in_use;

        /**
         * Retained diagnostics prefix.  Protocol parsing receives every
         * received byte through tg_txn_on_rx(), not only this prefix.
         */
        uint8_t response_prefix[TG_FLOW_RESPONSE_CAP];
        size_t response_prefix_len;
};

int tg_flow_map_init(struct tg_flow_map *map, uint16_t owner_lcore);
void tg_flow_map_fini(struct tg_flow_map *map);
void tg_flow_reset(struct tg_flow *flow);
int tg_flow_map_insert(struct tg_flow_map *map, struct tg_flow *flow,
                       struct nsock_handle handle);
struct tg_flow *tg_flow_map_lookup(const struct tg_flow_map *map,
                                   struct nsock_handle handle);
int tg_flow_map_remove(struct tg_flow_map *map, struct tg_flow *flow);

int tg_flow_start_tcp(struct tg_flow_map *map, struct tg_flow_pool *pool,
                      const struct sockaddr *peer, socklen_t peer_len,
                      const struct tg_proto_ops *proto,
                      const void *class_config);
void tg_flow_on_event(struct tg_flow_map *map, struct tg_flow_pool *pool,
                      struct tg_flow *flow, uint32_t events);

#endif /* TRAFFIC_GEN_FLOW_H */
