#ifndef TRAFFIC_GEN_FLOW_H
#define TRAFFIC_GEN_FLOW_H

/**
 * @file flow.h
 * @brief Owner-local traffic-generator flow identity and socket mapping.
 *
 * A traffic-generator flow retains an @ref nsock_handle instead of an nsock
 * pointer.  The handle's generation prevents a delayed readiness event for a
 * retired socket slot from being delivered to a flow using a reused slot.
 */

#include "../pro-stack/owner_io.h"

#include <stdbool.h>
#include <stdint.h>

/** Lifecycle state of one protocol transaction. */
enum tg_flow_state {
        TG_FLOW_NEW = 0,
        TG_FLOW_CONNECTING,
        TG_FLOW_SENDING,
        TG_FLOW_RECEIVING,
        TG_FLOW_DONE,
        TG_FLOW_FAILED,
};

struct tg_flow;
struct tg_conn_pool;

/**
 * Per-owner lookup table from socket slot id to traffic-generator flow.
 *
 * This table is accessed only by @ref owner_lcore.  Socket ids are bounded
 * and densely allocated by the owner, so direct indexing avoids a hash lookup
 * on the reactor hot path.  The table does not own its flow objects.
 */
struct tg_flow_map {
        struct tg_flow **by_socket_id;
        uint16_t owner_lcore;
};

/**
 * One owner-local protocol transaction.
 *
 * Future protocol implementations extend this object with request and response
 * buffers, deadlines, parser state, and per-flow statistics.
 */
struct tg_flow {
        struct nsock_handle handle;
        enum tg_flow_state state;

        /** True while this object is registered in a @ref tg_flow_map. */
        bool mapped;
        /** True while this object is checked out from its connection pool. */
        bool in_use;

        /* TODO: Add send buffer, receive buffer, deadline, and parser state. */
};

/**
 * Initialize a direct-index flow map for one packet-worker lcore.
 * @return 0 on success, or -1 with errno set on invalid input or allocation
 *         failure.
 */
int tg_flow_map_init(struct tg_flow_map *map, uint16_t owner_lcore);
/** Release flow-map storage after all flows have been removed. */
void tg_flow_map_fini(struct tg_flow_map *map);
/** Reset a reusable flow object to its unbound initial state. */
void tg_flow_reset(struct tg_flow *flow);

/**
 * Bind an unregistered flow to an owner-local socket handle.
 * @return 0 on success, or -1 with errno set when the handle is invalid, the
 *         flow is already mapped, or the socket slot is already occupied.
 */
int tg_flow_map_insert(struct tg_flow_map *map, struct tg_flow *flow,
                       struct nsock_handle handle);
/**
 * Resolve a readiness-event handle to its live flow, or return NULL.
 *
 * A NULL result is expected for stale events posted before flow teardown.
 */
struct tg_flow *tg_flow_map_lookup(const struct tg_flow_map *map,
                                   struct nsock_handle handle);
/**
 * Remove a flow's socket-slot mapping before closing its socket.
 * @return 0 on success, or -1 with errno set if the map no longer owns it.
 */
int tg_flow_map_remove(struct tg_flow_map *map, struct tg_flow *flow);

/**
 * Create and start one non-blocking TCP flow on the current owner lcore.
 *
 * The returned flow is registered in @p map before the TCP handshake starts.
 * A successful asynchronous start leaves the flow in TG_FLOW_CONNECTING.
 *
 * @return 0 when the connection attempt started, or -1 with errno set.
 */
int tg_flow_start_tcp(struct tg_flow_map *map, struct tg_conn_pool *pool,
                      const struct sockaddr *peer, socklen_t peer_len);

/**
 * Advance or tear down a flow from one coalesced readiness-event mask.
 *
 * The caller must have resolved @p flow through the matching owner-local map.
 * ERROR and HUP recycle the flow during this connection-validation stage.
 */
void tg_flow_on_event(struct tg_flow_map *map, struct tg_conn_pool *pool,
                      struct tg_flow *flow, uint32_t events);

#endif /* TRAFFIC_GEN_FLOW_H */