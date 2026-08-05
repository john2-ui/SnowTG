#ifndef TRAFFIC_GEN_FLOW_H
#define TRAFFIC_GEN_FLOW_H

/**
 * @file flow.h
 * @brief Owner-local TCP flow state, socket mapping, and lifecycle API.
 *
 * A flow is the transport owner for exactly one transaction.  It is mapped by
 * generation-qualified socket handle while active, then reports completion
 * before its transaction state is reset and the object re-enters its pool.
 */

#include "txn.h"

#include "../../pro-stack/owner_io.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/** @brief Number of response bytes retained solely for diagnostics. */
#define TG_FLOW_RESPONSE_CAP 4096U

/** @brief Transport lifecycle states for a short-lived TCP flow. */
enum tg_flow_state {
        /** @brief Pool-resident or newly reset flow without a socket. */
        TG_FLOW_NEW = 0,
        /** @brief Socket exists and asynchronous TCP establishment is pending.
         */
        TG_FLOW_CONNECTING,
        /** @brief Request has bytes remaining for nonblocking transmission. */
        TG_FLOW_SENDING,
        /** @brief Request is accepted and response bytes are being parsed. */
        TG_FLOW_RECEIVING,
        /** @brief Protocol completed successfully before object reclamation. */
        TG_FLOW_DONE,
        /** @brief Terminal transport or protocol failure before reclamation. */
        TG_FLOW_FAILED,
};

struct tg_flow;
struct tg_flow_pool;

/** @brief Stable terminal classifications reported to scheduler and stats. */
enum tg_flow_result {
        /** @brief Complete application response satisfied the protocol plugin.
         */
        TG_FLOW_RESULT_SUCCESS = 0,
        /** @brief Socket creation or TCP establishment did not complete. */
        TG_FLOW_RESULT_CONNECT_FAILURE,
        /** @brief Non-connect transport operation failed. */
        TG_FLOW_RESULT_IO_FAILURE,
        /** @brief Received bytes or EOF violated protocol requirements. */
        TG_FLOW_RESULT_PROTOCOL_FAILURE,
};

/**
 * @brief Observes one terminal flow result before the flow is reset.
 * @param ctx Opaque observer context supplied at flow creation.
 * @param flow Completed flow with transaction counters still available.
 * @param result Terminal result classified by the transport state machine.
 */
typedef void (*tg_flow_finish_fn)(void *ctx, const struct tg_flow *flow,
                                  enum tg_flow_result result);

/**
 * @brief Owner-local map from socket id to active generation-qualified flow.
 *
 * Lookup verifies the complete handle, not only the socket id, so a delayed
 * readiness event cannot be delivered to an object after socket-id reuse.
 */
struct tg_flow_map {
        struct tg_flow **by_socket_id;
        uint16_t owner_lcore;
};

/**
 * @brief Reusable state for one nonblocking TCP request/response exchange.
 *
 * Completion observers run before mapping removal, socket close, and pool
 * reset.  They must not retain @p flow because it may be reused immediately
 * after their callback returns.
 */
struct tg_flow {
        struct nsock_handle handle;
        enum tg_flow_state state;
        struct tg_txn txn;

        bool mapped;
        bool in_use;
        bool completion_notified;
        tg_flow_finish_fn on_finish;
        void *on_finish_ctx;

        /**
         * Retained diagnostics prefix.  Protocol parsing receives every
         * received byte through tg_txn_on_rx(), not only this prefix.
         */
        uint8_t response_prefix[TG_FLOW_RESPONSE_CAP];
        size_t response_prefix_len;
};

/**
 * @brief Allocates a socket-id map for the specified owner lcore.
 * @param map Destination map.
 * @param owner_lcore Lcore authorized to own inserted socket handles.
 * @return 0 on success; -1 with @c errno set otherwise.
 */
int tg_flow_map_init(struct tg_flow_map *map, uint16_t owner_lcore);

/** @brief Releases a flow map and clears its metadata. */
void tg_flow_map_fini(struct tg_flow_map *map);

/** @brief Resets a flow, including its protocol transaction state. */
void tg_flow_reset(struct tg_flow *flow);

/**
 * @brief Associates an active flow with a generation-qualified socket handle.
 * @return 0 on success; -1 if the map, ownership, or slot is invalid.
 */
int tg_flow_map_insert(struct tg_flow_map *map, struct tg_flow *flow,
                       struct nsock_handle handle);

/**
 * @brief Finds the flow matching an exact owner-I/O handle.
 * @return Matching flow, or @c NULL for invalid, stale, or unmapped handles.
 */
struct tg_flow *tg_flow_map_lookup(const struct tg_flow_map *map,
                                   struct nsock_handle handle);

/**
 * @brief Removes an active flow's socket-map association.
 * @return 0 on success; -1 if map membership invariants are not satisfied.
 */
int tg_flow_map_remove(struct tg_flow_map *map, struct tg_flow *flow);

/**
 * @brief Acquires, initializes, maps, and connects one nonblocking TCP flow.
 *
 * A successful return means the flow was admitted; connection completion may
 * still be pending.  Synchronous failures fully reclaim the pool object.
 *
 * @return 0 on admission; -1 with @c errno set if no flow or socket is usable.
 */
int tg_flow_start_tcp(struct tg_flow_map *map, struct tg_flow_pool *pool,
                      const struct sockaddr *peer, socklen_t peer_len,
                      const struct tg_proto_ops *proto,
                      const void *class_config, tg_flow_finish_fn on_finish,
                      void *on_finish_ctx);

/**
 * @brief Advances a flow from a coalesced owner-I/O readiness mask.
 *
 * The function drains writable and readable sockets to @c EAGAIN or terminal
 * state.  It can recycle @p flow, so callers must not dereference it after
 * this function returns.
 */
void tg_flow_on_event(struct tg_flow_map *map, struct tg_flow_pool *pool,
                      struct tg_flow *flow, uint32_t events);

#endif /* TRAFFIC_GEN_FLOW_H */
