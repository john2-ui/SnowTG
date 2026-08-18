#include "socket_owner.h"

#include "config.h"
#include "log.h"
#include "owner_io.h"
#include "socket.h"
#include "socket_owner_internal.h"
#include "tcp.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <rte_pause.h>
#include <rte_ring.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Every packet worker owns an independent slot table, command/ready queues,
 * and TCP memory domain.  Handles name their owner lcore, so application
 * commands can be routed without exposing an nsock pointer across lcores.
 */
static struct socket_owner g_owners[RTE_MAX_LCORE];
static bool g_owner_ready[RTE_MAX_LCORE];
static atomic_uint g_create_owner_next;

struct socket_ready_event {
        struct nsock_handle handle;
};

/**
 * Derive DPDK queue/pool capacities from the runtime slot capacity.
 *
 * rte_ring_create() uses the normal ring mode here, which requires a
 * power-of-two count.  The ready-event pool follows the same rounded value so
 * a full ready ring can always be backed by pool objects.
 */
static int socket_owner_capacity_params(uint32_t slot_capacity,
                                        uint32_t *command_capacity,
                                        uint32_t *ready_capacity) {
        uint64_t ready_requested;
        uint32_t command_count = 1;
        uint32_t ready_count = 1;

        if (slot_capacity == 0 || command_capacity == NULL ||
            ready_capacity == NULL)
                return -EINVAL;
        ready_requested = (uint64_t)slot_capacity * 2U;
        if (ready_requested > UINT32_MAX)
                return -EOVERFLOW;

        while (command_count < slot_capacity) {
                if (command_count > UINT32_MAX / 2U)
                        return -EOVERFLOW;
                command_count <<= 1;
        }
        while (ready_count < (uint32_t)ready_requested) {
                if (ready_count > UINT32_MAX / 2U)
                        return -EOVERFLOW;
                ready_count <<= 1;
        }

        *command_capacity = command_count < 2U ? 2U : command_count;
        *ready_capacity = ready_count < 2U ? 2U : ready_count;
        return 0;
}

static struct socket_owner *socket_owner_for_lcore(unsigned int lcore_id) {
        if (lcore_id >= RTE_MAX_LCORE || !g_owner_ready[lcore_id])
                return NULL;
        return &g_owners[lcore_id];
}

static struct socket_owner *socket_owner_current(void) {
        return socket_owner_for_lcore(rte_lcore_id());
}

static struct socket_owner *socket_owner_default(void) {
        unsigned int start = atomic_fetch_add_explicit(&g_create_owner_next, 1,
                                                       memory_order_relaxed);

        for (unsigned int offset = 0; offset < RTE_MAX_LCORE; offset++) {
                unsigned int lcore_id = (start + offset) % RTE_MAX_LCORE;
                struct socket_owner *owner = socket_owner_for_lcore(lcore_id);
                if (owner != NULL)
                        return owner;
        }
        return NULL;
}

/** Release partially created owner resources after an initialization failure.
 */
static void socket_owner_init_cleanup(struct socket_owner *owner) {
        if (owner == NULL)
                return;
        tcp_owner_memory_fini(&owner->tcp_memory);
        udp_owner_memory_fini(&owner->udp_memory);
        if (owner->ready_event_pool != NULL)
                rte_mempool_free(owner->ready_event_pool);
        if (owner->ready_ring != NULL)
                rte_ring_free(owner->ready_ring);
        if (owner->command_ring != NULL)
                rte_ring_free(owner->command_ring);
        free(owner->free_ids);
        free(owner->generations);
        free(owner->slots);
        memset(owner, 0, sizeof(*owner));
}

#define OWNER_SK_FMT "sock=%u gen=%u"
#define OWNER_SK_ARG(sk) (sk)->id, (sk)->generation

static const char *sock_cmd_type_str(enum sock_cmd_type type) {
        switch (type) {
        case SOCK_CMD_CREATE:
                return "create";
        case SOCK_CMD_BIND:
                return "bind";
        case SOCK_CMD_CONNECT:
                return "connect";
        case SOCK_CMD_LISTEN:
                return "listen";
        case SOCK_CMD_ACCEPT:
                return "accept";
        case SOCK_CMD_SEND:
                return "send";
        case SOCK_CMD_RECV:
                return "recv";
        case SOCK_CMD_SENDTO:
                return "sendto";
        case SOCK_CMD_RECVFROM:
                return "recvfrom";
        case SOCK_CMD_SETSOCKOPT:
                return "setsockopt";
        case SOCK_CMD_GETSOCKOPT:
                return "getsockopt";
        case SOCK_CMD_CLOSE:
                return "close";
        default:
                return "unknown";
        }
}

/** Resolve a handle only on the owner lcore; no pointer crosses this boundary.
 */
static struct nsock *owner_lookup(struct nsock_handle handle) {
        struct nsock *sk;
        struct socket_owner *owner = socket_owner_current();

        if (owner == NULL || owner->slots == NULL ||
            handle.id >= owner->slot_capacity ||
            handle.owner_lcore != owner->lcore_id)
                return NULL;

        sk = owner->slots[handle.id];
        if (sk == NULL || sk->generation != handle.generation ||
            sk->protocol != handle.protocol)
                return NULL;

        return sk;
}

struct nsock *socket_owner_resolve_local(struct nsock_handle handle) {
        struct socket_owner *owner = socket_owner_current();

        if (owner == NULL || handle.owner_lcore != owner->lcore_id) {
                errno = EPERM;
                return NULL;
        }

        struct nsock *sk = owner_lookup(handle);
        if (sk == NULL)
                errno = EBADF;
        return sk;
}

uint32_t socket_owner_slot_capacity_local(void) {
        struct socket_owner *owner = socket_owner_current();

        return owner == NULL ? 0 : owner->slot_capacity;
}

struct nsock *socket_owner_slot_at_local(uint32_t id) {
        struct socket_owner *owner = socket_owner_current();

        if (owner == NULL || owner->slots == NULL || id >= owner->slot_capacity)
                return NULL;
        return owner->slots[id];
}

/** Append a command to an owner-only FIFO wait queue. */
static void waitq_push(struct sock_cmd **head, struct sock_cmd **tail,
                       struct sock_cmd *cmd) {
        cmd->next = NULL;
        if (*tail != NULL)
                (*tail)->next = cmd;
        else
                *head = cmd;
        *tail = cmd;
}

/** Remove and return the first command from an owner-only wait queue. */
static struct sock_cmd *waitq_pop(struct sock_cmd **head,
                                  struct sock_cmd **tail) {
        struct sock_cmd *cmd = *head;
        if (cmd == NULL)
                return NULL;
        *head = cmd->next;
        if (*head == NULL)
                *tail = NULL;
        cmd->next = NULL;
        return cmd;
}

/** Restore a command at the front when a nonblocking owner probe hits EAGAIN.
 */
static void waitq_push_front(struct sock_cmd **head, struct sock_cmd **tail,
                             struct sock_cmd *cmd) {
        cmd->next = *head;
        *head = cmd;
        if (*tail == NULL)
                *tail = cmd;
}

int socket_owner_init_with_capacity(unsigned int lcore_id, uint32_t capacity) {
        struct socket_owner *owner;
        char command_name[RTE_RING_NAMESIZE];
        char ready_name[RTE_RING_NAMESIZE];
        char pool_name[RTE_MEMPOOL_NAMESIZE];
        uint32_t command_capacity;
        uint32_t ready_capacity;
        int capacity_rc;

        if (lcore_id >= RTE_MAX_LCORE) {
                errno = EINVAL;
                return -1;
        }
        capacity_rc = socket_owner_capacity_params(capacity, &command_capacity,
                                                   &ready_capacity);
        if (capacity_rc != 0) {
                errno = -capacity_rc;
                return -1;
        }
        if ((size_t)capacity > SIZE_MAX / sizeof(*g_owners[0].slots) ||
            (size_t)capacity > SIZE_MAX / sizeof(*g_owners[0].generations) ||
            (size_t)capacity > SIZE_MAX / sizeof(*g_owners[0].free_ids)) {
                errno = EOVERFLOW;
                return -1;
        }
        if (g_owner_ready[lcore_id]) {
                if (g_owners[lcore_id].slot_capacity == capacity)
                        return 0;
                errno = EBUSY;
                return -1;
        }

        owner = &g_owners[lcore_id];
        memset(owner, 0, sizeof(*owner));
        owner->lcore_id = lcore_id;
        owner->slot_capacity = capacity;
        owner->ready_capacity = ready_capacity;
        owner->slots = calloc(capacity, sizeof(*owner->slots));
        owner->generations = calloc(capacity, sizeof(*owner->generations));
        owner->free_ids = calloc(capacity, sizeof(*owner->free_ids));
        if (owner->slots == NULL || owner->generations == NULL ||
            owner->free_ids == NULL) {
                errno = ENOMEM;
                socket_owner_init_cleanup(owner);
                return -1;
        }
        for (uint32_t id = 0; id < capacity; id++)
                owner->free_ids[id] = capacity - id - 1U;
        owner->free_count = capacity;
        (void)snprintf(command_name, sizeof(command_name), "socket_commands_%u",
                       lcore_id);
        (void)snprintf(ready_name, sizeof(ready_name), "socket_ready_events_%u",
                       lcore_id);
        (void)snprintf(pool_name, sizeof(pool_name), "socket_ready_pool_%u",
                       lcore_id);

        /*
         * Every application lcore may submit commands, while exactly one
         * packet worker consumes them.  RING_F_SC_DEQ encodes only the latter;
         * enqueue therefore retains DPDK's multi-producer synchronization.
         */
        owner->command_ring = rte_ring_create(command_name, command_capacity,
                                              rte_socket_id(), RING_F_SC_DEQ);
        if (owner->command_ring == NULL) {
                LOG_OWNER_ERROR("owner command ring initialization failed");
                socket_owner_init_cleanup(owner);
                return -1;
        }

        owner->ready_ring =
            rte_ring_create(ready_name, ready_capacity, rte_socket_id(),
                            RING_F_SP_ENQ | RING_F_SC_DEQ);
        if (owner->ready_ring == NULL) {
                LOG_OWNER_ERROR("owner ready ring initialization failed");
                socket_owner_init_cleanup(owner);
                return -1;
        }

        owner->ready_event_pool = rte_mempool_create(
            pool_name, ready_capacity, sizeof(struct socket_ready_event), 0, 0,
            NULL, NULL, NULL, NULL, rte_socket_id(), 0);
        if (owner->ready_event_pool == NULL) {
                LOG_OWNER_ERROR("owner ready-event pool initialization failed");
                socket_owner_init_cleanup(owner);
                return -1;
        }
        if (tcp_owner_memory_init(&owner->tcp_memory, lcore_id) != 0) {
                LOG_OWNER_ERROR("owner TCP memory initialization failed");
                socket_owner_init_cleanup(owner);
                return -1;
        }
        if (udp_owner_memory_init(&owner->udp_memory, lcore_id) != 0) {
                LOG_OWNER_ERROR("owner UDP memory initialization failed");
                socket_owner_init_cleanup(owner);
                return -1;
        }

        tcp_ofo_metrics_reset_owner(lcore_id);
        g_owner_ready[lcore_id] = true;
        LOG_OWNER_INFO("event=init lcore=%u command_capacity=%u "
                       "ready_capacity=%u slot_capacity=%u",
                       lcore_id, command_capacity, ready_capacity, capacity);
        return 0;
}

int socket_owner_init(unsigned int lcore_id) {
        return socket_owner_init_with_capacity(lcore_id,
                                               NSOCK_ID_DEFAULT_CAPACITY);
}

void socket_owner_fini(void) {
        for (unsigned int lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
                if (!g_owner_ready[lcore_id])
                        continue;
                socket_owner_init_cleanup(&g_owners[lcore_id]);
                g_owner_ready[lcore_id] = false;
        }
}

/** @copydoc socket_owner_tcp_memory */
struct tcp_owner_memory *socket_owner_tcp_memory(void) {
        struct socket_owner *owner = socket_owner_current();

        if (owner == NULL)
                return NULL;
        return &owner->tcp_memory;
}

/** @copydoc socket_owner_udp_memory */
struct udp_owner_memory *socket_owner_udp_memory(void) {
        struct socket_owner *owner = socket_owner_current();

        if (owner == NULL)
                return NULL;
        return &owner->udp_memory;
}

/** @copydoc socket_owner_tcp_memory_snapshot */
int socket_owner_tcp_memory_snapshot(struct tcp_memory_snapshot *snapshot) {
        struct socket_owner *owner = socket_owner_current();

        if (snapshot == NULL || owner == NULL)
                return -1;
        tcp_owner_memory_snapshot(&owner->tcp_memory, snapshot);
        return 0;
}

/** @copydoc socket_owner_tcp_memory_below_low_water */
int socket_owner_tcp_memory_below_low_water(void) {
        struct socket_owner *owner = socket_owner_current();

        if (owner == NULL)
                return 1;
        return tcp_owner_memory_below_low_water(&owner->tcp_memory);
}

/** @copydoc socket_owner_tcp_memory_above_high_water */
int socket_owner_tcp_memory_above_high_water(void) {
        struct socket_owner *owner = socket_owner_current();

        if (owner == NULL)
                return 0;
        return tcp_owner_memory_above_high_water(&owner->tcp_memory);
}

int socket_owner_adopt(struct nsock *sk) {
        struct socket_owner *owner = socket_owner_current();

        if (owner == NULL || sk == NULL)
                return -EINVAL;

        if (owner->free_count == 0 || owner->free_ids == NULL)
                return -ENFILE;

        uint32_t id = owner->free_ids[--owner->free_count];
        /*
         * Retirement, rather than allocation, advances generation.  Thus
         * every handle becomes stale at the exact point its object is
         * unpublished; a reused slot receives the already advanced
         * generation.
         */
        uint32_t generation = owner->generations[id];
        if (generation == 0) {
                generation = 1;
                owner->generations[id] = generation;
        }

        sk->id = id;
        sk->generation = generation;
        sk->owner_lcore = (uint16_t)owner->lcore_id;
        owner->slots[id] = sk;
        LOG_OWNER_DEBUG(OWNER_SK_FMT " event=adopt owner_lcore=%u",
                        OWNER_SK_ARG(sk), sk->owner_lcore);
        return 0;
}

void socket_owner_retire(struct nsock *sk) {
        struct socket_owner *owner;

        if (sk == NULL || sk->id == NSOCK_INVALID_ID)
                return;
        owner = socket_owner_current();
        if (owner == NULL || sk->id >= owner->slot_capacity ||
            sk->owner_lcore != owner->lcore_id) {
                LOG_OWNER_ERROR("reject cross-owner retire socket=%u owner=%u "
                                "caller=%u",
                                sk->id, sk->owner_lcore, rte_lcore_id());
                return;
        }

        /*
         * Complete parked stack-resident commands before making the socket
         * unreachable.  Otherwise their application threads would wait
         * forever and their command storage could never be reclaimed.
         */
        socket_owner_abort_waiters(sk, ECANCELED);

        if (owner->slots[sk->id] == sk) {
                LOG_OWNER_DEBUG(OWNER_SK_FMT " event=retire", OWNER_SK_ARG(sk));
                owner->slots[sk->id] = NULL;
                uint32_t next = owner->generations[sk->id] + 1;
                /* Generation zero remains reserved after uint32_t wrap. */
                owner->generations[sk->id] = next == 0 ? 1 : next;
                if (owner->free_count < owner->slot_capacity)
                        owner->free_ids[owner->free_count++] = sk->id;
        }
}

struct nsock_handle socket_owner_handle(const struct nsock *sk) {
        struct nsock_handle handle = {
            .id = NSOCK_INVALID_ID,
        };
        if (sk == NULL)
                return handle;

        handle.id = sk->id;
        handle.generation = sk->generation;
        handle.owner_lcore = sk->owner_lcore;
        handle.protocol = sk->protocol;
        return handle;
}

void socket_owner_ready_post(struct nsock *sk, uint32_t events) {
        struct socket_ready_event *event;
        struct socket_owner *owner = socket_owner_current();

        if (sk == NULL || events == 0)
                return;
        if (owner == NULL || sk->owner_lcore != owner->lcore_id) {
                LOG_OWNER_ERROR("reject non-owner readiness post");
                return;
        }

        sk->ready_mask |= events;
        if (sk->ready_queued)
                return;

        if (rte_mempool_get(owner->ready_event_pool, (void **)&event) != 0) {
                LOG_OWNER_ERROR(OWNER_SK_FMT
                                " event=ready-drop reason=event-pool-empty",
                                OWNER_SK_ARG(sk));
                return;
        }

        event->handle = socket_owner_handle(sk);
        if (rte_ring_sp_enqueue(owner->ready_ring, event) != 0) {
                rte_mempool_put(owner->ready_event_pool, event);
                LOG_OWNER_ERROR(OWNER_SK_FMT
                                " event=ready-drop reason=ring-full",
                                OWNER_SK_ARG(sk));
                return;
        }

        sk->ready_queued = true;
}

unsigned int socket_owner_ready_burst(struct owner_io_event *events,
                                      unsigned int max_events) {
        unsigned int produced = 0;
        struct socket_owner *owner = socket_owner_current();

        if (events == NULL || max_events == 0 || owner == NULL)
                return 0;

        while (produced < max_events) {
                struct socket_ready_event *event;
                if (rte_ring_sc_dequeue(owner->ready_ring, (void **)&event) !=
                    0)
                        break;

                struct nsock *sk = owner_lookup(event->handle);
                if (sk != NULL) {
                        uint32_t mask = sk->ready_mask;
                        sk->ready_mask = 0;
                        sk->ready_queued = false;
                        if (mask != 0) {
                                events[produced].handle = event->handle;
                                events[produced].events = mask;
                                produced++;
                        }
                }
                rte_mempool_put(owner->ready_event_pool, event);
        }

        return produced;
}

void socket_owner_complete(struct sock_cmd *cmd, ssize_t result, int error) {
        pthread_mutex_lock(&cmd->done_mutex);
        cmd->result = result;
        cmd->error = error;
        cmd->done = true;
        pthread_cond_signal(&cmd->done_cond);
        pthread_mutex_unlock(&cmd->done_mutex);
}

int socket_owner_call(struct sock_cmd *cmd) {
        struct socket_owner *owner;

        if (cmd == NULL) {
                errno = EINVAL;
                return -1;
        }
        if (cmd->type == SOCK_CMD_CREATE)
                owner = socket_owner_default();
        else
                owner = socket_owner_for_lcore(cmd->handle.owner_lcore);
        if (owner == NULL) {
                errno = ENETDOWN;
                return -1;
        }
        if (pthread_mutex_init(&cmd->done_mutex, NULL) != 0) {
                errno = ENOMEM;
                return -1;
        }
        if (pthread_cond_init(&cmd->done_cond, NULL) != 0) {
                pthread_mutex_destroy(&cmd->done_mutex);
                errno = ENOMEM;
                return -1;
        }
        cmd->done = false;
        cmd->next = NULL;

        /*
         * Apply backpressure to command producers instead of returning
         * ENOBUFS.  In particular, nclose has already detached its fd before
         * submitting CLOSE; dropping that command would leave an unreachable
         * live TCB.  Waiting for ring space is consistent with the synchronous
         * API, which already waits for owner completion.
         */
        while (rte_ring_mp_enqueue(owner->command_ring, cmd) != 0)
                rte_pause();

        /*
         * Waiting here blocks only the application lcore.  A command that
         * cannot yet progress is retained by the owner, whose loop continues
         * to process packets and timers until it can complete the request.
         */
        pthread_mutex_lock(&cmd->done_mutex);
        while (!cmd->done)
                pthread_cond_wait(&cmd->done_cond, &cmd->done_mutex);
        pthread_mutex_unlock(&cmd->done_mutex);

        pthread_cond_destroy(&cmd->done_cond);
        pthread_mutex_destroy(&cmd->done_mutex);
        if (cmd->result < 0)
                errno = cmd->error;

        return cmd->result < 0 ? -1 : 0;
}

/** Convert legacy transport "-1 plus errno" into one command completion. */
static void complete_transport_result(struct sock_cmd *cmd, ssize_t result) {
        int error = result < 0 ? errno : 0;
        if (result < 0 && error == 0)
                error = EIO;
        socket_owner_complete(cmd, result, error);
}

void socket_owner_wake_recv(struct nsock *sk) {
        while (sk != NULL && sk->recv_wait_head != NULL) {
                /*
                 * Remove before probing. tcp_recv may drain OFO data, whose
                 * delivery recursively wakes another waiter; keeping the
                 * current command linked would execute it twice.
                 */
                struct sock_cmd *cmd =
                    waitq_pop(&sk->recv_wait_head, &sk->recv_wait_tail);
                ssize_t result;

                errno = 0;
                if (cmd->type == SOCK_CMD_RECV) {
                        result =
                            sk->ops->recv(sk, cmd->args.io.buf,
                                          cmd->args.io.len, cmd->args.io.flags);
                } else {
                        result = sk->ops->recvfrom(
                            sk, cmd->args.io.buf, cmd->args.io.len,
                            cmd->args.io.flags, cmd->args.io.out_addr,
                            cmd->args.io.out_addrlen);
                }

                if (result < 0 && errno == EAGAIN &&
                    !(cmd->args.io.flags & MSG_DONTWAIT)) {
                        LOG_OWNER_DEBUG(OWNER_SK_FMT " event=wait-park op=recv",
                                        OWNER_SK_ARG(sk));
                        waitq_push_front(&sk->recv_wait_head,
                                         &sk->recv_wait_tail, cmd);
                        return;
                }

                LOG_OWNER_DEBUG(
                    OWNER_SK_FMT " event=wait-wake op=recv result=%zd errno=%d",
                    OWNER_SK_ARG(sk), result, result < 0 ? errno : 0);
                complete_transport_result(cmd, result);
        }
}

void socket_owner_wake_send(struct nsock *sk) {
        while (sk != NULL && sk->send_wait_head != NULL) {
                struct sock_cmd *cmd = sk->send_wait_head;

                errno = 0;
                ssize_t result = sk->ops->send(
                    sk, cmd->args.io.buf, cmd->args.io.len, cmd->args.io.flags);
                if (result < 0 && errno == EAGAIN)
                        return;

                (void)waitq_pop(&sk->send_wait_head, &sk->send_wait_tail);
                complete_transport_result(cmd, result);
        }
}

void socket_owner_wake_accept(struct nsock *listener) {
        while (listener != NULL && listener->accept_wait_head != NULL) {
                struct nsock *child = tcp_accept_owned(listener);
                if (child == NULL) {
                        if (errno == EAGAIN)
                                return;

                        struct sock_cmd *failed =
                            waitq_pop(&listener->accept_wait_head,
                                      &listener->accept_wait_tail);
                        complete_transport_result(failed, -1);
                        continue;
                }

                struct sock_cmd *cmd = waitq_pop(&listener->accept_wait_head,
                                                 &listener->accept_wait_tail);
                child->app_visible = true;
                cmd->result_handle = socket_owner_handle(child);
                if (cmd->args.address.out_addr != NULL) {
                        struct sockaddr_in *sin =
                            (struct sockaddr_in *)cmd->args.address.out_addr;
                        sin->sin_family = AF_INET;
                        sin->sin_port = child->u.tcp.remote_port;
                        sin->sin_addr.s_addr = child->u.tcp.remote_ip;
                        if (cmd->args.address.out_addrlen != NULL)
                                *cmd->args.address.out_addrlen = sizeof(*sin);
                }
                socket_owner_complete(cmd, 0, 0);
        }
}

void socket_owner_complete_connect(struct nsock *sk, int error) {
        if (sk == NULL || sk->connect_waiter == NULL)
                return;
        struct sock_cmd *cmd = sk->connect_waiter;
        sk->connect_waiter = NULL;
        socket_owner_complete(cmd, error == 0 ? 0 : -1, error);
}

void socket_owner_abort_waiters(struct nsock *sk, int error) {
        struct sock_cmd *cmd;
        unsigned int aborted = 0;
        if (sk == NULL)
                return;

        if (sk->connect_waiter != NULL) {
                cmd = sk->connect_waiter;
                sk->connect_waiter = NULL;
                socket_owner_complete(cmd, -1, error);
                aborted++;
        }

        while ((cmd = waitq_pop(&sk->recv_wait_head, &sk->recv_wait_tail)) !=
               NULL) {
                socket_owner_complete(cmd, -1, error);
                aborted++;
        }
        while ((cmd = waitq_pop(&sk->send_wait_head, &sk->send_wait_tail)) !=
               NULL) {
                socket_owner_complete(cmd, -1, error);
                aborted++;
        }
        while ((cmd = waitq_pop(&sk->accept_wait_head,
                                &sk->accept_wait_tail)) != NULL) {
                socket_owner_complete(cmd, -1, error);
                aborted++;
        }
        if (aborted > 0)
                LOG_OWNER_WARN(OWNER_SK_FMT
                               " event=wait-abort count=%u errno=%d",
                               OWNER_SK_ARG(sk), aborted, error);
}

/** Handle one request without ever sleeping on protocol progress. */
static void owner_process_one(struct sock_cmd *cmd) {
        struct nsock *sk = NULL;
        ssize_t result;

        if (cmd->type == SOCK_CMD_CREATE) {
                int adopt_rc = 0;

                sk = nsock_alloc((uint8_t)cmd->args.create.protocol);
                if (sk != NULL)
                        adopt_rc = socket_owner_adopt(sk);
                if (sk == NULL || adopt_rc != 0) {
                        if (sk != NULL)
                                nsock_free(sk);
                        socket_owner_complete(
                            cmd, -1, adopt_rc != 0 ? -adopt_rc : ENOMEM);
                        return;
                }
                sk->app_visible = true;
                cmd->result_handle = socket_owner_handle(sk);
                socket_owner_complete(cmd, 0, 0);
                return;
        }

        sk = owner_lookup(cmd->handle);
        if (sk == NULL) {
                LOG_OWNER_WARN("event=invalid-handle op=%s sock=%u gen=%u "
                               "owner_lcore=%u protocol=%u",
                               sock_cmd_type_str(cmd->type), cmd->handle.id,
                               cmd->handle.generation, cmd->handle.owner_lcore,
                               cmd->handle.protocol);
                socket_owner_complete(cmd, -1, EBADF);
                return;
        }
        if (sk->app_closed && cmd->type != SOCK_CMD_CLOSE) {
                LOG_OWNER_WARN(OWNER_SK_FMT " event=closed-handle op=%s",
                               OWNER_SK_ARG(sk), sock_cmd_type_str(cmd->type));
                socket_owner_complete(cmd, -1, EBADF);
                return;
        }

        errno = 0;
        switch (cmd->type) {
        case SOCK_CMD_BIND: {
                const struct sockaddr_in *sin =
                    (const struct sockaddr_in *)&cmd->args.address.addr;
                result =
                    nsock_bind_local(sk, sin->sin_addr.s_addr, sin->sin_port);
                if (result < 0) {
                        errno = -result;
                        result = -1;
                }
                complete_transport_result(cmd, result);
                return;
        }
        case SOCK_CMD_CONNECT:
                if (sk->ops->connect == NULL || sk->connect_waiter != NULL) {
                        socket_owner_complete(cmd, -1,
                                              sk->connect_waiter ? EALREADY
                                                                 : EOPNOTSUPP);
                        return;
                }
                result = sk->ops->connect(
                    sk, (const struct sockaddr *)&cmd->args.address.addr,
                    cmd->args.address.addrlen);
                if (result < 0 && errno == EINPROGRESS) {
                        sk->connect_waiter = cmd;
                        LOG_OWNER_DEBUG(OWNER_SK_FMT
                                        " event=wait-park op=connect",
                                        OWNER_SK_ARG(sk));
                        return;
                }
                complete_transport_result(cmd, result);
                return;
        case SOCK_CMD_LISTEN:
                if (sk->ops->listen == NULL) {
                        socket_owner_complete(cmd, -1, EOPNOTSUPP);
                        return;
                }
                complete_transport_result(
                    cmd, sk->ops->listen(sk, cmd->args.listen.backlog));
                return;
        case SOCK_CMD_ACCEPT:
                if (sk->ops->accept == NULL) {
                        socket_owner_complete(cmd, -1, EOPNOTSUPP);
                        return;
                }
                waitq_push(&sk->accept_wait_head, &sk->accept_wait_tail, cmd);
                LOG_OWNER_DEBUG(OWNER_SK_FMT " event=wait-park op=accept",
                                OWNER_SK_ARG(sk));
                socket_owner_wake_accept(sk);
                return;
        case SOCK_CMD_SEND:
                if (sk->ops->send == NULL) {
                        socket_owner_complete(cmd, -1, EOPNOTSUPP);
                        return;
                }
                result = sk->ops->send(sk, cmd->args.io.buf, cmd->args.io.len,
                                       cmd->args.io.flags);
                if (result < 0 && errno == EAGAIN &&
                    !(cmd->args.io.flags & MSG_DONTWAIT)) {
                        waitq_push(&sk->send_wait_head, &sk->send_wait_tail,
                                   cmd);
                        LOG_OWNER_DEBUG(OWNER_SK_FMT " event=wait-park op=send",
                                        OWNER_SK_ARG(sk));
                        return;
                }
                complete_transport_result(cmd, result);
                return;
        case SOCK_CMD_RECV:
        case SOCK_CMD_RECVFROM:
                if ((cmd->type == SOCK_CMD_RECV && sk->ops->recv == NULL) ||
                    (cmd->type == SOCK_CMD_RECVFROM &&
                     sk->ops->recvfrom == NULL)) {
                        socket_owner_complete(cmd, -1, EOPNOTSUPP);
                        return;
                }
                waitq_push(&sk->recv_wait_head, &sk->recv_wait_tail, cmd);
                LOG_OWNER_DEBUG(OWNER_SK_FMT " event=wait-park op=%s",
                                OWNER_SK_ARG(sk), sock_cmd_type_str(cmd->type));
                socket_owner_wake_recv(sk);
                return;
        case SOCK_CMD_SENDTO:
                if (sk->ops->sendto == NULL) {
                        socket_owner_complete(cmd, -1, EOPNOTSUPP);
                        return;
                }
                complete_transport_result(
                    cmd,
                    sk->ops->sendto(sk, cmd->args.io.buf, cmd->args.io.len,
                                    cmd->args.io.flags,
                                    (const struct sockaddr *)&cmd->args.io.addr,
                                    cmd->args.io.addrlen));
                return;
        case SOCK_CMD_SETSOCKOPT:
                if (sk->protocol != IPPROTO_TCP ||
                    cmd->args.sockopt.level != SOL_SOCKET ||
                    cmd->args.sockopt.optname != SO_LINGER) {
                        socket_owner_complete(cmd, -1, ENOPROTOOPT);
                        return;
                }
                sk->u.tcp.linger_enabled =
                    cmd->args.sockopt.value.l_onoff != 0;
                sk->u.tcp.linger_seconds =
                    (uint32_t)cmd->args.sockopt.value.l_linger;
                socket_owner_complete(cmd, 0, 0);
                return;
        case SOCK_CMD_GETSOCKOPT:
                if (sk->protocol != IPPROTO_TCP ||
                    cmd->args.sockopt.level != SOL_SOCKET ||
                    cmd->args.sockopt.optname != SO_LINGER) {
                        socket_owner_complete(cmd, -1, ENOPROTOOPT);
                        return;
                }
                cmd->args.sockopt.out_value->l_onoff =
                    sk->u.tcp.linger_enabled ? 1 : 0;
                cmd->args.sockopt.out_value->l_linger =
                    (int)sk->u.tcp.linger_seconds;
                *cmd->args.sockopt.out_len = sizeof(struct linger);
                socket_owner_complete(cmd, 0, 0);
                return;
        case SOCK_CMD_CLOSE:
                if (sk->app_closed) {
                        socket_owner_complete(cmd, -1, EBADF);
                        return;
                }
                sk->app_closed = true;
                socket_owner_abort_waiters(sk, ECANCELED);
                if (sk->ops->close == NULL) {
                        socket_owner_complete(cmd, -1, EOPNOTSUPP);
                        return;
                }
                /*
                 * close() is an ownership transfer, not a wait for TIME_WAIT.
                 * The transport starts teardown and may immediately destroy
                 * UDP/listener sockets; do not dereference sk afterwards.
                 */
                result = sk->ops->close(sk);
                complete_transport_result(cmd, result);
                return;
        default:
                socket_owner_complete(cmd, -1, EINVAL);
                return;
        }
}

void socket_owner_process_commands(void) {
        struct sock_cmd *commands[BURST_SIZE];
        struct socket_owner *owner = socket_owner_current();

        if (owner == NULL)
                return;
        unsigned int count = rte_ring_sc_dequeue_burst(
            owner->command_ring, (void **)commands, BURST_SIZE, NULL);

        for (unsigned int i = 0; i < count; i++)
                owner_process_one(commands[i]);
}
