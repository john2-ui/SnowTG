#include "flow.h"
#include "conn_pool.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/** Return true when @p handle can address an entry in @p map. */
static bool tg_flow_handle_valid_for_map(const struct tg_flow_map *map,
                                         struct nsock_handle handle) {
        return map != NULL && map->by_socket_id != NULL &&
               handle.id < NSOCK_ID_MAX &&
               handle.owner_lcore == map->owner_lcore;
}

/** Compare every field that identifies one socket lifetime. */
static bool tg_flow_handle_equal(struct nsock_handle left,
                                 struct nsock_handle right) {
        return left.id == right.id && left.owner_lcore == right.owner_lcore &&
               left.generation == right.generation &&
               left.protocol == right.protocol;
}

int tg_flow_map_init(struct tg_flow_map *map, uint16_t owner_lcore) {
        if (map == NULL) {
                errno = EINVAL;
                return -1;
        }

        memset(map, 0, sizeof(*map));

        /*
         * Socket ids are allocated in [0, NSOCK_ID_MAX).  Direct indexing is
         * deterministic and avoids an rte_hash lookup in every reactor event.
         */
        map->by_socket_id = calloc(NSOCK_ID_MAX, sizeof(*map->by_socket_id));
        if (map->by_socket_id == NULL) {
                errno = ENOMEM;
                return -1;
        }

        map->owner_lcore = owner_lcore;
        return 0;
}

void tg_flow_map_fini(struct tg_flow_map *map) {
        if (map == NULL)
                return;

        free(map->by_socket_id);
        memset(map, 0, sizeof(*map));
}

void tg_flow_reset(struct tg_flow *flow) {
        if (flow == NULL) {
                return;
        }

        memset(flow, 0, sizeof(*flow));
        flow->handle.id = NSOCK_INVALID_ID;
        flow->state = TG_FLOW_NEW;
}

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
        /*
         * Publish only after the flow contains its complete handle.  The
         * reactor runs on this same owner lcore, so no atomic operation is
         * required.
         */
        map->by_socket_id[handle.id] = flow;
        return 0;
}

struct tg_flow *tg_flow_map_lookup(const struct tg_flow_map *map,
                                   const struct nsock_handle handle) {
        struct tg_flow *flow;

        if (!tg_flow_handle_valid_for_map(map, handle)) {
                return NULL;
        }

        flow = map->by_socket_id[handle.id];
        if (flow == NULL || !flow->mapped ||
            !tg_flow_handle_equal(flow->handle, handle)) {
                return NULL;
        }

        return flow;
}

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

        /*
         * Teardown removes this entry before owner_io_close().  A HUP or ERROR
         * event posted by the close path then resolves to NULL instead of a
         * recycled flow object.
         */
        map->by_socket_id[flow->handle.id] = NULL;
        flow->mapped = false;
        flow->handle.id = NSOCK_INVALID_ID;
        return 0;
}

/**
 * Return a flow to its pool after a socket-creation or connection-start error.
 *
 * The socket-slot mapping must disappear before closing the socket.  Close may
 * post a final HUP or ERROR event, which then safely resolves as stale.
 */
static void tg_flow_start_cleanup(struct tg_flow_map *map,
                                  struct tg_conn_pool *pool,
                                  struct tg_flow *flow, bool socket_created) {
        struct nsock_handle handle = flow->handle;
        int saved_errno = errno;

        if (flow->mapped)
                (void)tg_flow_map_remove(map, flow);

        if (socket_created)
                (void)owner_io_close(handle);

        (void)tg_conn_pool_put(pool, flow);
        errno = saved_errno;
}

int tg_flow_start_tcp(struct tg_flow_map *map, struct tg_conn_pool *pool,
                      const struct sockaddr *peer, socklen_t peer_len,
                      const void *request, size_t request_len) {
        struct tg_flow *flow;
        struct nsock_handle handle;

        if (map == NULL || pool == NULL || peer == NULL || request == NULL ||
            request_len == 0 || request_len > sizeof(flow->request)) {
                errno = EINVAL;
                return -1;
        }

        flow = tg_conn_pool_get(pool);
        if (flow == NULL) {
                return -1;
        }

        /*
         * A flow owns its request bytes because a future scheduler may build
         * requests in temporary storage before starting a connection.
         */
        memcpy(flow->request, request, request_len);
        flow->request_len = request_len;

        if (owner_io_socket_create(IPPROTO_TCP, &handle) != 0) {
                tg_flow_start_cleanup(map, pool, flow, false);
                return -1;
        }

        if (tg_flow_map_insert(map, flow, handle) != 0) {
                /*
                 * Insert failure means no event can resolve to this flow, but
                 * the newly created socket must still be closed.
                 */
                flow->handle = handle;
                tg_flow_start_cleanup(map, pool, flow, true);
                return -1;
        }

        flow->state = TG_FLOW_CONNECTING;

        if (owner_io_connect(flow->handle, peer, peer_len) == 0) {
                /*
                 * The current TCP implementation normally returns EINPROGRESS.
                 * Preserve this branch for a future transport that completes
                 * a connection immediately.
                 */

                flow->state = TG_FLOW_SENDING;
                return 0;
        }

        if (errno == EINPROGRESS) {
                return 0;
        }

        tg_flow_start_cleanup(map, pool, flow, true);
        return -1;
}

/**
 * Push pending application bytes into the owner-local TCP send buffer.
 *
 * @return 0 when blocked by normal backpressure or finished, or -1 for a
 *         terminal transport failure.  A successful return with every byte
 *         accepted transitions the flow to TG_FLOW_RECEIVING.
 */
static int tg_flow_send_pending(struct tg_flow *flow) {
        while (flow->request_offset < flow->request_len) {
                size_t remaining = flow->request_len - flow->request_offset;
                ssize_t sent = owner_io_send(
                    flow->handle, flow->request + flow->request_offset,
                    remaining);
                if (sent > 0) {
                        flow->request_offset += (size_t)sent;
                        continue;
                }

                if (sent < 0 && errno == EAGAIN)
                        return 0;

                /*
                 * tcp_send() returns zero only for a zero-length request,
                 * which cannot occur while request_offset < request_len.
                 */
                if (sent == 0)
                        errno = EIO;
                return -1;
        }

        flow->state = TG_FLOW_RECEIVING;
        return 0;
}

/**
 * Drain every currently readable TCP byte into the flow response prefix.
 *
 * The owner-ready queue coalesces READ notifications.  The reactor therefore
 * must continue reading until EAGAIN; stopping after one read could leave
 * bytes queued without another readiness notification.
 *
 * @return 0 on EAGAIN or EOF, or -1 for a terminal receive error.
 */
static int tg_flow_drain_receive(struct tg_flow *flow, bool *eof_out) {
        uint8_t chunk[1024];

        *eof_out = false;

        for (;;) {
                ssize_t received =
                    owner_io_recv(flow->handle, chunk, sizeof(chunk));

                if (received > 0) {
                        size_t bytes = (size_t)received;
                        size_t retained = 0;

                        flow->response_bytes += bytes;

                        if (flow->response_len < sizeof(flow->response)) {
                                retained =
                                    sizeof(flow->response) - flow->response_len;
                                if (retained > bytes)
                                        retained = bytes;

                                memcpy(flow->response + flow->response_len,
                                       chunk, retained);
                                flow->response_len += retained;
                        }
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
 * Remove a flow from the socket map, close its socket, and return it to pool.
 *
 * The socket handle is copied before map removal because map removal
 * intentionally invalidates flow->handle to prevent accidental reuse.
 */

static void tg_flow_recycle(struct tg_flow_map *map, struct tg_conn_pool *pool,
                            struct tg_flow *flow) {
        struct nsock_handle handle = flow->handle;

        if (flow->mapped)
                (void)tg_flow_map_remove(map, flow);

        (void)owner_io_close(handle);
        (void)tg_conn_pool_put(pool, flow);
}

void tg_flow_on_event(struct tg_flow_map *map, struct tg_conn_pool *pool,
                      struct tg_flow *flow, uint32_t events) {
        if (map == NULL || pool == NULL || flow == NULL)
                return;

        /*
         * A connection failure takes precedence over every other coalesced
         * event bit.  The socket cannot make forward progress after ERROR.
         */
        if (events & OWNER_IO_EV_ERROR) {
                flow->state = TG_FLOW_FAILED;
                tg_flow_recycle(map, pool, flow);
                return;
        }

        if (events & OWNER_IO_EV_CONNECTED) {
                /*
                 * The current TCP path posts CONNECTED after receiving a valid
                 * SYN+ACK and transitioning the TCB to ESTABLISHED.
                 */
                if (flow->state == TG_FLOW_CONNECTING) {
                        flow->state = TG_FLOW_SENDING;
                }
        }

        /*
         * CONNECTED provides the first send opportunity.  Subsequent WRITE
         * notifications mean TCP ACK/window progress freed local sndbuf space.
         */
        if (flow->state == TG_FLOW_SENDING &&
            (events & (OWNER_IO_EV_CONNECTED | OWNER_IO_EV_WRITE))) {
                if (tg_flow_send_pending(flow) != 0) {
                        flow->state = TG_FLOW_FAILED;
                        tg_flow_recycle(map, pool, flow);
                        return;
                }
        }
        /*
         * TCP is full duplex: a peer may send response or error bytes before
         * all request bytes have entered the local send buffer.  Drain READ in
         * both SENDING and RECEIVING states; only WRITE advances the pending
         * request.
         *
         * For HUP | READ, drain first so queued response bytes are retained
         * before EOF completes the flow.
         */
        if ((flow->state == TG_FLOW_SENDING ||
             flow->state == TG_FLOW_RECEIVING) &&
            (events & (OWNER_IO_EV_READ | OWNER_IO_EV_HUP))) {
                bool eof;

                if (tg_flow_drain_receive(flow, &eof) != 0) {
                        flow->state = TG_FLOW_FAILED;
                        tg_flow_recycle(map, pool, flow);
                        return;
                }

                if (eof) {
                        flow->state = TG_FLOW_DONE;
                        tg_flow_recycle(map, pool, flow);
                        return;
                }
        }

        /*
         * A TCP HUP must expose EOF after queued response bytes are drained.
         * Reaching this branch means the transport contract was not met, so
         * retire the flow instead of keeping it indefinitely.
         */
        if (events & OWNER_IO_EV_HUP) {
                flow->state = TG_FLOW_FAILED;
                tg_flow_recycle(map, pool, flow);
        }
}