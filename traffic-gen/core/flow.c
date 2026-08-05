/**
 * @file flow.c
 * @brief Implements the owner-local nonblocking TCP flow state machine.
 *
 * A flow maps a generation-qualified owner-I/O socket to one protocol
 * transaction.  Terminal handling notifies observers before deleting the map,
 * closing the socket, and returning the object to its fixed-capacity pool.
 */

#include "flow.h"
#include "flow_pool.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

/** @brief Checks whether a handle belongs to this map's owner and id range. */
static bool tg_flow_handle_valid_for_map(const struct tg_flow_map *map,
                                         struct nsock_handle handle) {
        return map != NULL && map->by_socket_id != NULL &&
               handle.id < NSOCK_ID_MAX &&
               handle.owner_lcore == map->owner_lcore;
}

/** @brief Compares every handle field used to reject stale readiness events. */
static bool tg_flow_handle_equal(struct nsock_handle left,
                                 struct nsock_handle right) {
        return left.id == right.id && left.owner_lcore == right.owner_lcore &&
               left.generation == right.generation &&
               left.protocol == right.protocol;
}

/** @copydoc tg_flow_map_init */
int tg_flow_map_init(struct tg_flow_map *map, uint16_t owner_lcore) {
        if (map == NULL) {
                errno = EINVAL;
                return -1;
        }

        memset(map, 0, sizeof(*map));
        map->by_socket_id = calloc(NSOCK_ID_MAX, sizeof(*map->by_socket_id));
        if (map->by_socket_id == NULL) {
                errno = ENOMEM;
                return -1;
        }

        map->owner_lcore = owner_lcore;
        return 0;
}

/** @copydoc tg_flow_map_fini */
void tg_flow_map_fini(struct tg_flow_map *map) {
        if (map == NULL)
                return;

        free(map->by_socket_id);
        memset(map, 0, sizeof(*map));
}

/** @copydoc tg_flow_reset */
void tg_flow_reset(struct tg_flow *flow) {
        if (flow == NULL)
                return;

        tg_txn_reset(&flow->txn);
        memset(flow, 0, sizeof(*flow));
        flow->handle.id = NSOCK_INVALID_ID;
        flow->state = TG_FLOW_NEW;
}

/** @copydoc tg_flow_map_insert */
int tg_flow_map_insert(struct tg_flow_map *map, struct tg_flow *flow,
                       struct nsock_handle handle) {
        if (flow == NULL || !tg_flow_handle_valid_for_map(map, handle)) {
                errno = EINVAL;
                return -1;
        }
        if (flow->mapped) {
                errno = EALREADY;
                return -1;
        }
        if (map->by_socket_id[handle.id] != NULL) {
                errno = EEXIST;
                return -1;
        }

        flow->handle = handle;
        flow->mapped = true;
        map->by_socket_id[handle.id] = flow;
        return 0;
}

/** @copydoc tg_flow_map_lookup */
struct tg_flow *tg_flow_map_lookup(const struct tg_flow_map *map,
                                   const struct nsock_handle handle) {
        struct tg_flow *flow;

        if (!tg_flow_handle_valid_for_map(map, handle))
                return NULL;

        flow = map->by_socket_id[handle.id];
        if (flow == NULL || !flow->mapped ||
            !tg_flow_handle_equal(flow->handle, handle))
                return NULL;

        return flow;
}

/** @copydoc tg_flow_map_remove */
int tg_flow_map_remove(struct tg_flow_map *map, struct tg_flow *flow) {
        if (map == NULL || flow == NULL || !flow->mapped ||
            !tg_flow_handle_valid_for_map(map, flow->handle)) {
                errno = EINVAL;
                return -1;
        }
        if (map->by_socket_id[flow->handle.id] != flow) {
                errno = ENOENT;
                return -1;
        }

        map->by_socket_id[flow->handle.id] = NULL;
        flow->mapped = false;
        flow->handle.id = NSOCK_INVALID_ID;
        return 0;
}

/**
 * @brief Reclaims a partially initialized flow while preserving @c errno.
 *
 * This path deliberately does not invoke the completion observer: admission
 * never succeeded, so the caller records the synchronous start failure.
 */
static void tg_flow_start_cleanup(struct tg_flow_map *map,
                                  struct tg_flow_pool *pool,
                                  struct tg_flow *flow, bool socket_created) {
        struct nsock_handle handle = flow->handle;
        int saved_errno = errno;

        if (flow->mapped)
                (void)tg_flow_map_remove(map, flow);
        if (socket_created)
                (void)owner_io_close(handle);
        (void)tg_flow_pool_put(pool, flow);
        errno = saved_errno;
}

/** @copydoc tg_flow_start_tcp */
int tg_flow_start_tcp(struct tg_flow_map *map, struct tg_flow_pool *pool,
                      const struct sockaddr *peer, socklen_t peer_len,
                      const struct tg_proto_ops *proto,
                      const void *class_config, tg_flow_finish_fn on_finish,
                      void *on_finish_ctx,
                      tg_flow_socket_created_fn on_socket_created,
                      owner_io_release_fn on_socket_released,
                      void *socket_lifecycle_ctx) {
        struct tg_flow *flow;
        struct nsock_handle handle;

        if (map == NULL || pool == NULL || peer == NULL || proto == NULL) {
                errno = EINVAL;
                return -1;
        }

        flow = tg_flow_pool_get(pool);
        if (flow == NULL)
                return -1;
        flow->on_finish = on_finish;
        flow->on_finish_ctx = on_finish_ctx;

        if (tg_txn_init(&flow->txn, proto, class_config) != 0) {
                (void)tg_flow_pool_put(pool, flow);
                return -1;
        }
        if (owner_io_socket_create_local(IPPROTO_TCP, &handle) != 0) {
                tg_flow_start_cleanup(map, pool, flow, false);
                return -1;
        }
        if (owner_io_set_release_observer(handle, on_socket_released,
                                          socket_lifecycle_ctx) != 0) {
                tg_flow_start_cleanup(map, pool, flow, true);
                return -1;
        }
        if (on_socket_created != NULL)
                on_socket_created(socket_lifecycle_ctx);
        if (tg_flow_map_insert(map, flow, handle) != 0) {
                flow->handle = handle;
                tg_flow_start_cleanup(map, pool, flow, true);
                return -1;
        }

        flow->state = TG_FLOW_CONNECTING;
        if (owner_io_connect(flow->handle, peer, peer_len) == 0) {
                flow->state = TG_FLOW_SENDING;
                return 0;
        }
        if (errno == EINPROGRESS)
                return 0;

        tg_flow_start_cleanup(map, pool, flow, true);
        return -1;
}

/**
 * @brief Drains the serialized request to transport until @c EAGAIN or done.
 * @return 0 if transmission remains pending or completes; -1 on I/O error.
 */
static int tg_flow_send_pending(struct tg_flow *flow) {
        while (flow->txn.request_offset < flow->txn.request_len) {
                size_t remaining =
                    flow->txn.request_len - flow->txn.request_offset;
                ssize_t sent = owner_io_send(
                    flow->handle, flow->txn.request + flow->txn.request_offset,
                    remaining);

                if (sent > 0) {
                        flow->txn.request_offset += (size_t)sent;
                        tg_txn_on_tx_accepted(&flow->txn, (size_t)sent);
                        continue;
                }
                if (sent < 0 && errno == EAGAIN)
                        return 0;
                if (sent == 0)
                        errno = EIO;
                return -1;
        }

        flow->state = TG_FLOW_RECEIVING;
        return 0;
}

/**
 * @brief Drains received bytes, retains diagnostics, and feeds the protocol.
 *
 * @p complete_out denotes parser completion before EOF; @p eof_out denotes
 * clean transport EOF, which requires separate protocol finalization.
 */
static int tg_flow_drain_receive(struct tg_flow *flow, bool *eof_out,
                                 bool *complete_out) {
        uint8_t chunk[1024];

        *eof_out = false;
        *complete_out = false;
        for (;;) {
                ssize_t received =
                    owner_io_recv(flow->handle, chunk, sizeof(chunk));
                if (received > 0) {
                        enum tg_proto_result result;
                        size_t bytes = (size_t)received;
                        size_t retained = 0;

                        if (flow->response_prefix_len <
                            sizeof(flow->response_prefix)) {
                                retained = sizeof(flow->response_prefix) -
                                           flow->response_prefix_len;
                                if (retained > bytes)
                                        retained = bytes;
                                memcpy(flow->response_prefix +
                                           flow->response_prefix_len,
                                       chunk, retained);
                                flow->response_prefix_len += retained;
                        }

                        result = tg_txn_on_rx(&flow->txn, chunk, bytes);
                        if (result == TG_PROTO_FAILED) {
                                errno = EPROTO;
                                return -1;
                        }
                        if (result == TG_PROTO_COMPLETE)
                                *complete_out = true;
                        continue;
                }
                if (received == 0) {
                        *eof_out = true;
                        return 0;
                }
                if (errno == EAGAIN)
                        return 0;
                return -1;
        }
}

/**
 * @brief Emits the terminal callback once, then unmaps, closes, and pools flow.
 */
static void tg_flow_recycle(struct tg_flow_map *map, struct tg_flow_pool *pool,
                            struct tg_flow *flow, enum tg_flow_result result) {
        struct nsock_handle handle = flow->handle;

        if (!flow->completion_notified && flow->on_finish != NULL) {
                flow->completion_notified = true;
                flow->on_finish(flow->on_finish_ctx, flow, result);
        }
        if (flow->mapped)
                (void)tg_flow_map_remove(map, flow);
        (void)owner_io_close(handle);
        (void)tg_flow_pool_put(pool, flow);
}

/** @copydoc tg_flow_on_event */
void tg_flow_on_event(struct tg_flow_map *map, struct tg_flow_pool *pool,
                      struct tg_flow *flow, uint32_t events) {
        if (map == NULL || pool == NULL || flow == NULL)
                return;

        if (events & OWNER_IO_EV_ERROR) {
                bool connecting = flow->state == TG_FLOW_CONNECTING;

                flow->state = TG_FLOW_FAILED;
                tg_flow_recycle(map, pool, flow,
                                connecting ? TG_FLOW_RESULT_CONNECT_FAILURE
                                           : TG_FLOW_RESULT_IO_FAILURE);
                return;
        }

        if ((events & OWNER_IO_EV_CONNECTED) &&
            flow->state == TG_FLOW_CONNECTING)
                flow->state = TG_FLOW_SENDING;

        if (flow->state == TG_FLOW_SENDING &&
            (events & (OWNER_IO_EV_CONNECTED | OWNER_IO_EV_WRITE))) {
                if (tg_flow_send_pending(flow) != 0) {
                        flow->state = TG_FLOW_FAILED;
                        tg_flow_recycle(map, pool, flow,
                                        TG_FLOW_RESULT_IO_FAILURE);
                        return;
                }
        }

        if ((flow->state == TG_FLOW_SENDING ||
             flow->state == TG_FLOW_RECEIVING) &&
            (events & (OWNER_IO_EV_READ | OWNER_IO_EV_HUP))) {
                bool eof;
                bool complete;

                if (tg_flow_drain_receive(flow, &eof, &complete) != 0) {
                        flow->state = TG_FLOW_FAILED;
                        tg_flow_recycle(map, pool, flow,
                                        TG_FLOW_RESULT_PROTOCOL_FAILURE);
                        return;
                }
                if (complete) {
                        flow->state = TG_FLOW_DONE;
                        tg_flow_recycle(map, pool, flow,
                                        TG_FLOW_RESULT_SUCCESS);
                        return;
                }
                if (eof) {
                        if (tg_txn_on_eof(&flow->txn) == TG_PROTO_COMPLETE)
                                flow->state = TG_FLOW_DONE;
                        else
                                flow->state = TG_FLOW_FAILED;
                        tg_flow_recycle(map, pool, flow,
                                        flow->state == TG_FLOW_DONE
                                            ? TG_FLOW_RESULT_SUCCESS
                                            : TG_FLOW_RESULT_PROTOCOL_FAILURE);
                        return;
                }
        }

        if (events & OWNER_IO_EV_HUP) {
                bool connecting = flow->state == TG_FLOW_CONNECTING;

                flow->state = TG_FLOW_FAILED;
                tg_flow_recycle(map, pool, flow,
                                connecting ? TG_FLOW_RESULT_CONNECT_FAILURE
                                           : TG_FLOW_RESULT_IO_FAILURE);
        }
}
