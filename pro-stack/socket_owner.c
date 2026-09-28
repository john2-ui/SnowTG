/**
 * @file socket_owner.c
 * @brief Owner-serialized commands, cancellation, and coalesced transport
 * events.
 *
 * Application descriptors are deep-copied before submission. A managed request
 * holds one caller reference and, after enqueue, one owner reference. Returning
 * or cancelled callers transfer their reference to the allocation-free control
 * list; only the owner unlinks parked work and retires the pair of references.
 * CREATE/ACCEPT retain the caller reference until fd publication is settled.
 * Timers borrow the owner reference and are cancelled before final reclamation.
 */
#include "socket_owner.h"
#include "tcp_ofo.h"

#include "config.h"
#include "log.h"
#include "net_context.h"
#include "owner_io.h"
#include "socket.h"
#include "socket_owner_internal.h"
#include "socket_public_internal.h"
#include "tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/*
 * Every packet worker owns an independent slot table, command/ready queues,
 * and TCP memory domain.  Handles name their owner lcore, so application
 * commands can be routed without exposing an nsock pointer across lcores.
 */
static struct socket_owner g_owners[RTE_MAX_LCORE];
static atomic_bool g_owner_ready[RTE_MAX_LCORE];
static pthread_mutex_t submit_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint g_create_owner_next;
static atomic_uint_fast64_t command_live;
uint64_t socket_owner_command_live(void) { return atomic_load(&command_live); }

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
        unsigned count = 0;
        for (unsigned i = 0; i < RTE_MAX_LCORE; i++)
                if (g_owner_ready[i] && g_owners[i].accepting)
                        count++;
        if (!count)
                return NULL;
        unsigned selected = atomic_fetch_add_explicit(&g_create_owner_next, 1,
                                                      memory_order_relaxed) %
                            count;
        for (unsigned i = 0; i < RTE_MAX_LCORE; i++)
                if (g_owner_ready[i] && g_owners[i].accepting &&
                    selected-- == 0)
                        return &g_owners[i];
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
        if (owner->close_ring != NULL)
                rte_ring_free(owner->close_ring);
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
        case SOCK_CMD_FLAGS:
                return "flags";
        case SOCK_CMD_PUBLISH:
                return "publish";
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
        cmd->state = SOCK_CMD_PARKED;
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
        cmd->state = SOCK_CMD_PARKED;
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
        owner->slot_resources.capacity = capacity;
        owner->ready_resources.capacity = ready_capacity;
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

        snprintf(command_name, sizeof(command_name), "socket_close_%u",
                 lcore_id);
        owner->close_ring = rte_ring_create(command_name, command_capacity,
                                            rte_socket_id(), RING_F_SC_DEQ);
        if (owner->close_ring == NULL) {
                socket_owner_init_cleanup(owner);
                return -1;
        }
        owner->accepting = true;
        atomic_init(&owner->space_seq, 0);
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
        pthread_mutex_lock(&submit_lock);
        for (unsigned int lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
                if (!g_owner_ready[lcore_id])
                        continue;
                socket_owner_init_cleanup(&g_owners[lcore_id]);
                g_owner_ready[lcore_id] = false;
        }
        pthread_mutex_unlock(&submit_lock);
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

        if (owner->free_count == 0 || owner->free_ids == NULL) {
                if (owner->free_ids == NULL)
                        owner->slot_resources.unavailable++;
                else
                        owner->slot_resources.exhausted++;
                return -ENFILE;
        }

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
        resource_acquire(&owner->slot_resources, 1);
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
         * Complete parked managed commands before making the socket
         * unreachable.  Otherwise their application threads would wait
         * forever and their command storage could never be reclaimed.
         */
        socket_owner_abort_waiters(sk, ECANCELED);

        if (owner->slots[sk->id] == sk) {
                LOG_OWNER_DEBUG(OWNER_SK_FMT " event=retire", OWNER_SK_ARG(sk));
                owner->slots[sk->id] = NULL;
                resource_release(&owner->slot_resources, 1);
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

        if (sk->protocol == IPPROTO_UDP && (events & OWNER_IO_EV_WRITE))
                socket_owner_wake_send(sk);
        socket_public_refresh(sk);
        sk->ready_mask |= events;
        if (sk->ready_queued)
                return;

        if (rte_mempool_get(owner->ready_event_pool, (void **)&event) != 0) {
                owner->ready_resources.exhausted++;
                owner->ready_overflow = true;
                LOG_OWNER_DEBUG(OWNER_SK_FMT
                                " event=ready-deferred reason=event-pool-empty",
                                OWNER_SK_ARG(sk));
                return;
        }

        resource_acquire(&owner->ready_resources, 1);
        event->handle = socket_owner_handle(sk);
        if (rte_ring_sp_enqueue(owner->ready_ring, event) != 0) {
                owner->ready_resources.limit++;
                rte_mempool_put(owner->ready_event_pool, event);
                resource_release(&owner->ready_resources, 1);
                owner->ready_overflow = true;
                LOG_OWNER_DEBUG(OWNER_SK_FMT
                                " event=ready-deferred reason=ring-full",
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
                resource_release(&owner->ready_resources, 1);
        }

        if (owner->ready_overflow) {
                uint32_t scanned = 0;
                while (scanned < owner->slot_capacity &&
                       produced < max_events) {
                        struct nsock *sk = owner->slots[owner->ready_scan];
                        owner->ready_scan =
                            (owner->ready_scan + 1) % owner->slot_capacity;
                        scanned++;
                        if (sk && sk->ready_mask && !sk->ready_queued) {
                                events[produced++] = (struct owner_io_event){
                                    socket_owner_handle(sk), sk->ready_mask};
                                sk->ready_mask = 0;
                                owner->ready_recoveries++;
                        }
                }
                if (scanned == owner->slot_capacity)
                        owner->ready_overflow = false;
        }
        return produced;
}

/** @brief Read the monotonic application deadline clock in nanoseconds. */
static uint64_t command_now(void) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/** @brief Publish queue progress before waking producers; the sequence closes
 * the check/wait race. */
static void space_wake(struct socket_owner *owner) {
        atomic_fetch_add_explicit(&owner->space_seq, 1, memory_order_release);
        syscall(SYS_futex, &owner->space_seq, FUTEX_WAKE_PRIVATE, INT_MAX, NULL,
                NULL, 0);
}

/** @brief Release one reference; no queue, timer or caller may access the final
 * object afterward. */
static void command_put(struct sock_cmd *cmd) {
        if (atomic_fetch_sub(&cmd->refs, 1) != 1)
                return;
        atomic_fetch_sub(&command_live, 1);
        free(cmd->storage);
        pthread_cond_destroy(&cmd->done_cond);
        pthread_mutex_destroy(&cmd->done_mutex);
        free(cmd);
}

void socket_owner_cancel(struct sock_cmd *cmd, int error) {
        int expected = 0;
        if (!atomic_compare_exchange_strong(&cmd->cancel_error, &expected,
                                            error ? error : ECANCELED))
                return;
        /* Cancellation must progress even while its caller keeps waiting and
         * no packet arrives. The first request owns one intrusive notification
         * and reference; repeated cancellation cannot grow the control queue.
         */
        pthread_mutex_lock(&submit_lock);
        if (cmd->owner && cmd->owner->accepting) {
                atomic_fetch_add(&cmd->refs, 1);
                cmd->cancel_next = cmd->owner->cancel_head;
                cmd->owner->cancel_head = cmd;
                atomic_store_explicit(&cmd->owner->cancel_pending, true,
                                      memory_order_release);
        }
        pthread_mutex_unlock(&submit_lock);
}

void socket_owner_complete(struct sock_cmd *cmd, ssize_t result, int error) {
        owner_timer_cancel(&cmd->timer);
        struct nsock *sk = owner_lookup(cmd->handle);
        if (sk)
                socket_public_refresh(sk);
        pthread_mutex_lock(&cmd->done_mutex);
        if (!cmd->done) {
                cmd->result = result;
                cmd->error = error;
                cmd->state =
                    result < 0 && (error == ECANCELED || error == ETIMEDOUT)
                        ? SOCK_CMD_CANCELLED
                        : SOCK_CMD_DONE;
                cmd->done = true;
                pthread_cond_broadcast(&cmd->done_cond);
        }
        pthread_mutex_unlock(&cmd->done_mutex);
}

/** @brief Unlink a parked request on its owner without disturbing FIFO order.
 */
static void command_unlink_wait(struct nsock *sk, struct sock_cmd *cmd) {
        struct sock_cmd **heads[] = {&sk->recv_wait_head, &sk->send_wait_head,
                                     &sk->accept_wait_head};
        struct sock_cmd **tails[] = {&sk->recv_wait_tail, &sk->send_wait_tail,
                                     &sk->accept_wait_tail};
        for (unsigned i = 0; i < 3; i++) {
                struct sock_cmd *prev = NULL;
                for (struct sock_cmd *it = *heads[i]; it; it = it->next) {
                        if (it == cmd) {
                                if (prev)
                                        prev->next = it->next;
                                else
                                        *heads[i] = it->next;
                                if (*tails[i] == it)
                                        *tails[i] = prev;
                                it->next = NULL;
                                return;
                        }
                        prev = it;
                }
        }
        if (sk->connect_waiter == cmd)
                sk->connect_waiter = NULL;
}

/** @brief Commit cancellation on the owner; completed side effects always win.
 */
static void command_cancel_local(struct sock_cmd *cmd, int error) {
        if (cmd->done)
                return;
        struct nsock *sk = owner_lookup(cmd->handle);
        bool connecting = sk && sk->connect_waiter == cmd;
        if (sk)
                command_unlink_wait(sk, cmd);
        cmd->owner->command_cancels++;
        socket_owner_complete(cmd, -1, error);
        if (connecting)
                tcp_force_abort(sk, error, "command-cancel");
}

/** @brief Expire one owner-parked request without retaining application memory.
 */
static void command_timeout(struct owner_timer *timer, void *arg,
                            uint64_t now) {
        (void)timer;
        (void)now;
        command_cancel_local(arg, ETIMEDOUT);
}

/* Transfer the caller reference into an allocation-free control notification.
 * The owner retains a separate reference until this notification is consumed.
 */
static void command_release_caller(struct sock_cmd *cmd) {
        pthread_mutex_lock(&submit_lock);
        if (cmd->owner && cmd->owner->accepting) {
                cmd->control_next = cmd->owner->control_head;
                cmd->owner->control_head = cmd;
                atomic_store_explicit(&cmd->owner->control_pending, true,
                                      memory_order_release);
                pthread_mutex_unlock(&submit_lock);
        } else {
                pthread_mutex_unlock(&submit_lock);
                command_put(cmd);
        }
}

void socket_owner_result_release(struct sock_cmd *descriptor, bool claimed) {
        struct sock_cmd *cmd = descriptor->managed_result;
        if (!cmd)
                return;
        descriptor->managed_result = NULL;
        cmd->claimed = claimed;
        command_release_caller(cmd);
}

static void command_submission_cleanup(void *arg) { command_put(arg); }

static void command_wait_cleanup(void *arg) {
        struct sock_cmd *cmd = arg;
        /* pthread_cond_wait reacquires this mutex before invoking cleanup. */
        pthread_mutex_unlock(&cmd->done_mutex);
        socket_owner_cancel(cmd, ECANCELED);
        command_release_caller(cmd);
}

static void command_active_remove(struct sock_cmd *cmd) {
        struct socket_owner *owner = cmd->owner;
        if (cmd->active_prev)
                cmd->active_prev->active_next = cmd->active_next;
        else
                owner->active_head = cmd->active_next;
        if (cmd->active_next)
                cmd->active_next->active_prev = cmd->active_prev;
}

/** @brief Consume the transferred caller reference and close any unclaimed
 * result. */
static void command_reap(struct sock_cmd *cmd) {
        if (!cmd->done)
                command_cancel_local(cmd, atomic_load(&cmd->cancel_error)
                                              ?: ECANCELED);
        if (!cmd->claimed && cmd->result >= 0 &&
            (cmd->type == SOCK_CMD_CREATE || cmd->type == SOCK_CMD_ACCEPT)) {
                struct nsock *sk = owner_lookup(cmd->result_handle);
                if (sk && !sk->app_closed) {
                        sk->app_closed = true;
                        sk->ops->close(sk);
                }
        }
        command_active_remove(cmd);
        command_put(cmd); /* owner */
        command_put(cmd); /* transferred caller */
}

/** @brief Deep-copy caller-owned buffers before a request can escape to its
 * owner. */
static struct sock_cmd *command_clone(const struct sock_cmd *src) {
        struct sock_cmd *cmd = calloc(1, sizeof(*cmd));
        if (!cmd) {
                errno = ENOMEM;
                return NULL;
        }
        cmd->type = src->type;
        cmd->handle = src->handle;
        cmd->args = src->args;
        cmd->nonblock = src->nonblock;
        cmd->timeout_ns = src->timeout_ns;
        cmd->result_handle.id = NSOCK_INVALID_ID;
        atomic_init(&cmd->refs, 1);
        atomic_init(&cmd->cancel_error, 0);
        owner_timer_init(&cmd->timer, command_timeout, cmd);
        if (pthread_mutex_init(&cmd->done_mutex, NULL)) {
                free(cmd);
                errno = ENOMEM;
                return NULL;
        }
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        int rc = pthread_cond_init(&cmd->done_cond, &attr);
        pthread_condattr_destroy(&attr);
        if (rc) {
                pthread_mutex_destroy(&cmd->done_mutex);
                free(cmd);
                errno = rc;
                return NULL;
        }
        atomic_fetch_add(&command_live, 1);
        if (cmd->type == SOCK_CMD_SEND || cmd->type == SOCK_CMD_RECV ||
            cmd->type == SOCK_CMD_SENDTO || cmd->type == SOCK_CMD_RECVFROM) {
                size_t len = cmd->args.io.len;
                bool datagram = cmd->handle.protocol == IPPROTO_UDP;
                if (datagram && len > 65507 && cmd->type == SOCK_CMD_SENDTO) {
                        command_put(cmd);
                        errno = EMSGSIZE;
                        return NULL;
                }
                if (len > (datagram ? 65507U : 65536U))
                        len = datagram ? 65507U : 65536U;
                cmd->storage = malloc(len ? len : 1);
                if (!cmd->storage) {
                        command_put(cmd);
                        errno = ENOMEM;
                        return NULL;
                }
                cmd->args.io.buf = cmd->storage;
                cmd->args.io.len = len;
                if (len && (cmd->type == SOCK_CMD_SEND ||
                            cmd->type == SOCK_CMD_SENDTO))
                        memcpy(cmd->storage, src->args.io.buf, len);
                cmd->output_len = sizeof(cmd->output_addr);
                cmd->args.io.out_addr = (struct sockaddr *)&cmd->output_addr;
                cmd->args.io.out_addrlen = &cmd->output_len;
        } else if (cmd->type == SOCK_CMD_ACCEPT) {
                cmd->output_len = sizeof(cmd->output_addr);
                cmd->args.address.out_addr =
                    (struct sockaddr *)&cmd->output_addr;
                cmd->args.address.out_addrlen = &cmd->output_len;
        } else if (cmd->type == SOCK_CMD_GETSOCKOPT) {
                cmd->output_len = sizeof(cmd->args.sockopt.time);
                cmd->args.sockopt.out_value = &cmd->args.sockopt.value;
                cmd->args.sockopt.out_len = &cmd->output_len;
        }
        return cmd;
}

static void copy_address(struct sockaddr *dst, socklen_t *len,
                         const struct sock_cmd *cmd) {
        if (!dst || !len)
                return;
        size_t n = *len < cmd->output_len ? *len : cmd->output_len;
        memcpy(dst, &cmd->output_addr, n);
        *len = cmd->output_len;
}

int socket_owner_call(struct sock_cmd *src) {
        if (!src) {
                errno = EINVAL;
                return -1;
        }
        struct sock_cmd *cmd = command_clone(src);
        if (!cmd)
                return -1;
        int old_cancel;
        /* Submission owns no owner reference until enqueue succeeds. */
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel);
        if (cmd->timeout_ns) {
                uint64_t now = command_now();
                cmd->deadline_ns = UINT64_MAX - now < cmd->timeout_ns
                                       ? UINT64_MAX
                                       : now + cmd->timeout_ns;
        }
        int error = 0;
        for (;;) {
                pthread_mutex_lock(&submit_lock);
                struct socket_owner *owner = cmd->owner;
                if (!owner) {
                        owner = cmd->type == SOCK_CMD_CREATE
                                    ? socket_owner_default()
                                    : socket_owner_for_lcore(
                                          cmd->handle.owner_lcore);
                        cmd->owner = owner;
                }
                if (!owner || !owner->accepting) {
                        pthread_mutex_unlock(&submit_lock);
                        error = ENETDOWN;
                        break;
                }
                struct rte_ring *ring = cmd->type == SOCK_CMD_CLOSE
                                            ? owner->close_ring
                                            : owner->command_ring;
                unsigned seq = atomic_load(&owner->space_seq);
                cmd->state = SOCK_CMD_QUEUED;
                atomic_fetch_add(&cmd->refs, 1);
                if (rte_ring_mp_enqueue(ring, cmd) == 0) {
                        unsigned depth = rte_ring_count(ring);
                        if (depth > owner->command_peak)
                                owner->command_peak = depth;
                        pthread_mutex_unlock(&submit_lock);
                        break;
                }
                atomic_fetch_sub(&cmd->refs, 1);
                owner->command_waits++;
                pthread_mutex_unlock(&submit_lock);
                if (cmd->nonblock) {
                        error = EAGAIN;
                        break;
                }
                if (cmd->deadline_ns && command_now() >= cmd->deadline_ns) {
                        error = ETIMEDOUT;
                        break;
                }
                /* Bounded sleep also permits pending pthread cancellation
                 * while a full ring has not yet acquired our command. */
                struct timespec delay = {.tv_nsec = 10000000};
                pthread_cleanup_push(command_submission_cleanup, cmd);
                pthread_setcancelstate(old_cancel, NULL);
                syscall(SYS_futex, &owner->space_seq, FUTEX_WAIT_PRIVATE, seq,
                        &delay, NULL, 0);
                pthread_testcancel();
                pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
                pthread_cleanup_pop(0);
        }
        if (error) {
                command_put(cmd);
                pthread_setcancelstate(old_cancel, NULL);
                errno = error;
                return -1;
        }
        pthread_mutex_lock(&cmd->done_mutex);
        pthread_cleanup_push(command_wait_cleanup, cmd);
        pthread_setcancelstate(old_cancel, NULL);
        while (!cmd->done) {
                if (cmd->deadline_ns && !atomic_load(&cmd->cancel_error)) {
                        struct timespec deadline = {
                            .tv_sec = cmd->deadline_ns / 1000000000ULL,
                            .tv_nsec = cmd->deadline_ns % 1000000000ULL};
                        if (pthread_cond_timedwait(&cmd->done_cond,
                                                   &cmd->done_mutex,
                                                   &deadline) == ETIMEDOUT)
                                socket_owner_cancel(cmd, ETIMEDOUT);
                } else {
                        pthread_cond_wait(&cmd->done_cond, &cmd->done_mutex);
                }
        }
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        bool holds_result = cmd->result >= 0 && (cmd->type == SOCK_CMD_CREATE ||
                                                 cmd->type == SOCK_CMD_ACCEPT);
        cmd->claimed = !holds_result;
        if (holds_result)
                src->managed_result = cmd;
        src->result = cmd->result;
        src->error = cmd->error;
        src->result_handle = cmd->result_handle;
        if (cmd->result >= 0) {
                if (cmd->type == SOCK_CMD_RECV ||
                    cmd->type == SOCK_CMD_RECVFROM) {
                        if (cmd->result)
                                memcpy(src->args.io.buf, cmd->storage,
                                       (size_t)cmd->result);
                        copy_address(src->args.io.out_addr,
                                     src->args.io.out_addrlen, cmd);
                } else if (cmd->type == SOCK_CMD_ACCEPT) {
                        copy_address(src->args.address.out_addr,
                                     src->args.address.out_addrlen, cmd);
                } else if (cmd->type == SOCK_CMD_GETSOCKOPT) {
                        size_t n = *src->args.sockopt.out_len;
                        if (n > cmd->output_len)
                                n = cmd->output_len;
                        memcpy(src->args.sockopt.out_value,
                               &cmd->args.sockopt.value, n);
                        *src->args.sockopt.out_len = cmd->output_len;
                }
        }
        pthread_cleanup_pop(0);
        pthread_mutex_unlock(&cmd->done_mutex);
        error = cmd->error;
        if (!src->managed_result)
                command_release_caller(cmd);
        pthread_setcancelstate(old_cancel, NULL);
        if (src->result < 0)
                errno = error;
        return src->result < 0 ? -1 : 0;
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
                if (atomic_load(&cmd->cancel_error)) {
                        command_cancel_local(cmd,
                                             atomic_load(&cmd->cancel_error));
                        continue;
                }
                cmd->state = SOCK_CMD_RUNNING;
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

                if (result < 0 && errno == EAGAIN && !cmd->nonblock &&
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
                if (atomic_load(&cmd->cancel_error)) {
                        command_cancel_local(cmd,
                                             atomic_load(&cmd->cancel_error));
                        continue;
                }
                errno = 0;
                ssize_t result =
                    cmd->type == SOCK_CMD_SENDTO
                        ? sk->ops->sendto(sk, cmd->args.io.buf,
                                          cmd->args.io.len, cmd->args.io.flags,
                                          (struct sockaddr *)&cmd->args.io.addr,
                                          cmd->args.io.addrlen)
                        : sk->ops->send(sk, cmd->args.io.buf, cmd->args.io.len,
                                        cmd->args.io.flags);
                if (result < 0 && errno == EAGAIN)
                        return;

                (void)waitq_pop(&sk->send_wait_head, &sk->send_wait_tail);
                complete_transport_result(cmd, result);
        }
}

void socket_owner_wake_accept(struct nsock *listener) {
        while (listener != NULL && listener->accept_wait_head != NULL) {
                struct sock_cmd *waiting = listener->accept_wait_head;
                if (atomic_load(&waiting->cancel_error)) {
                        command_cancel_local(
                            waiting, atomic_load(&waiting->cancel_error));
                        continue;
                }
                struct nsock *child = tcp_accept_owned(listener);
                if (child == NULL) {
                        if (errno == EAGAIN &&
                            !listener->accept_wait_head->nonblock)
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
                child->nonblock =
                    (cmd->args.address.flags & SOCK_NONBLOCK) != 0;
                child->nodelay = listener->nodelay;
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
        if (sk == NULL)
                return;
        if (error)
                sk->pending_error = error;
        if (sk->connect_waiter == NULL)
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
        if (error != ECANCELED)
                sk->pending_error = error;

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

/** @brief Read or update owner-local socket options; no output points into
 * caller storage. */
static int socket_option(struct nsock *sk, struct sock_cmd *cmd) {
        bool set = cmd->type == SOCK_CMD_SETSOCKOPT;
        int level = cmd->args.sockopt.level, name = cmd->args.sockopt.optname;
        int *value = &cmd->args.sockopt.integer;
        if (level == SOL_SOCKET) {
                if (name == SO_ERROR && !set) {
                        *value = sk->pending_error;
                        sk->pending_error = 0;
                        cmd->output_len = sizeof(int);
                        return 0;
                }
                if (name == SO_REUSEADDR) {
                        if (set && sk->registry_flags)
                                return EINVAL;
                        if (set)
                                sk->reuseaddr = *value != 0;
                        else
                                *value = sk->reuseaddr;
                        cmd->output_len = sizeof(int);
                        return 0;
                }
                if (name == SO_RCVTIMEO || name == SO_SNDTIMEO) {
                        uint64_t *ns = name == SO_RCVTIMEO
                                           ? &sk->recv_timeout_ns
                                           : &sk->send_timeout_ns;
                        struct timeval *tv = &cmd->args.sockopt.time;
                        if (set)
                                *ns = (uint64_t)tv->tv_sec * 1000000000ULL +
                                      (uint64_t)tv->tv_usec * 1000;
                        else
                                *tv = (struct timeval){
                                    .tv_sec = *ns / 1000000000ULL,
                                    .tv_usec = (*ns % 1000000000ULL) / 1000};
                        cmd->output_len = sizeof(*tv);
                        return 0;
                }
                if (name == SO_LINGER && sk->protocol == IPPROTO_TCP) {
                        if (set) {
                                sk->u.tcp.linger_enabled =
                                    cmd->args.sockopt.value.l_onoff != 0;
                                sk->u.tcp.linger_seconds =
                                    cmd->args.sockopt.value.l_linger;
                        } else {
                                cmd->args.sockopt.value =
                                    (struct linger){sk->u.tcp.linger_enabled,
                                                    sk->u.tcp.linger_seconds};
                        }
                        cmd->output_len = sizeof(struct linger);
                        return 0;
                }
        } else if (level == IPPROTO_TCP && name == TCP_NODELAY &&
                   sk->protocol == IPPROTO_TCP) {
                if (set) {
                        sk->nodelay = *value != 0;
                        if (sk->nodelay)
                                nsock_tx_mark_dirty(sk);
                } else
                        *value = sk->nodelay;
                cmd->output_len = sizeof(int);
                return 0;
        }
        return ENOPROTOOPT;
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
                sk->nonblock = (cmd->args.create.type & SOCK_NONBLOCK) != 0;
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
                if (sk->protocol == IPPROTO_TCP &&
                    sk->u.tcp.status != TCP_STATUS_CLOSED) {
                        socket_owner_complete(
                            cmd, -1,
                            sk->u.tcp.status == TCP_STATUS_SYN_SENT ? EALREADY
                                                                    : EISCONN);
                        return;
                }
                if (sk->ops->connect == NULL || sk->connect_waiter != NULL) {
                        socket_owner_complete(cmd, -1,
                                              sk->connect_waiter ? EALREADY
                                                                 : EOPNOTSUPP);
                        return;
                }
                result = sk->ops->connect(
                    sk, (const struct sockaddr *)&cmd->args.address.addr,
                    cmd->args.address.addrlen);
                if (result < 0 && errno == EINPROGRESS && !cmd->nonblock) {
                        cmd->state = SOCK_CMD_PARKED;
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
                if (result < 0 && errno == EAGAIN && !cmd->nonblock &&
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
                if (g_net.ipv4_mtu <= 28 ||
                    cmd->args.io.len > g_net.ipv4_mtu - 28U) {
                        socket_owner_complete(cmd, -1, EMSGSIZE);
                        return;
                }
                result = sk->ops->sendto(sk, cmd->args.io.buf, cmd->args.io.len,
                                         cmd->args.io.flags,
                                         (struct sockaddr *)&cmd->args.io.addr,
                                         cmd->args.io.addrlen);
                if (result < 0 && errno == EAGAIN && !cmd->nonblock) {
                        waitq_push(&sk->send_wait_head, &sk->send_wait_tail,
                                   cmd);
                        return;
                }
                complete_transport_result(cmd, result);
                return;
        case SOCK_CMD_SETSOCKOPT:
        case SOCK_CMD_GETSOCKOPT: {
                int error = socket_option(sk, cmd);
                socket_owner_complete(cmd, error ? -1 : 0, error);
                return;
        }
        case SOCK_CMD_PUBLISH:
                sk->public_fd = cmd->args.sockopt.integer;
                socket_owner_complete(cmd, 0, 0);
                return;
        case SOCK_CMD_FLAGS:
                if (cmd->args.sockopt.optname == F_SETFL)
                        sk->nonblock =
                            (cmd->args.sockopt.integer & O_NONBLOCK) != 0;
                socket_owner_complete(cmd, sk->nonblock ? O_NONBLOCK : 0, 0);
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

/** @brief Acquire owner-list membership, enforce the admission deadline, then
 * execute. */
static void command_start(struct socket_owner *owner, struct sock_cmd *cmd) {
        cmd->active_next = owner->active_head;
        if (owner->active_head)
                owner->active_head->active_prev = cmd;
        owner->active_head = cmd;
        cmd->state = SOCK_CMD_RUNNING;
        int error = atomic_load(&cmd->cancel_error);
        if (!error && cmd->deadline_ns && command_now() >= cmd->deadline_ns)
                error = ETIMEDOUT;
        if (error) {
                command_cancel_local(cmd, error);
                return;
        }
        if (cmd->deadline_ns) {
                uint64_t now = command_now();
                uint64_t remaining =
                    cmd->deadline_ns > now ? cmd->deadline_ns - now : 1;
                if (owner_timer_arm_after_ms(
                        &cmd->timer, remaining / 1000000 +
                                         (remaining % 1000000 != 0)) != 0) {
                        socket_owner_complete(cmd, -1, ENOBUFS);
                        return;
                }
        }
        owner_process_one(cmd);
}

/** Drain independent cancellation notifications. A queued command still owns
 * its ring reference and observes cancel_error when dequeued; parked commands
 * complete immediately without requiring ingress or caller abandonment. */
static void command_cancellations(struct socket_owner *owner) {
        if (!atomic_load_explicit(&owner->cancel_pending, memory_order_acquire))
                return;
        for (unsigned i = 0; i < BURST_SIZE; i++) {
                pthread_mutex_lock(&submit_lock);
                struct sock_cmd *cmd = owner->cancel_head;
                if (cmd)
                        owner->cancel_head = cmd->cancel_next;
                atomic_store_explicit(&owner->cancel_pending,
                                      owner->cancel_head != NULL,
                                      memory_order_release);
                pthread_mutex_unlock(&submit_lock);
                if (!cmd)
                        break;
                if (cmd->state != SOCK_CMD_QUEUED)
                        command_cancel_local(cmd,
                                             atomic_load(&cmd->cancel_error));
                command_put(cmd); /* cancellation notification */
        }
}

/** @brief Drain a bounded control batch, deferring requests still held by a
 * data ring. */
static void command_controls(struct socket_owner *owner) {
        /* The traffic generator has no public callers: its owner loop must
         * not contend on a process-wide mutex just to observe an empty list. */
        if (!atomic_load_explicit(&owner->control_pending,
                                  memory_order_acquire))
                return;
        pthread_mutex_lock(&submit_lock);
        struct sock_cmd *list = owner->control_head;
        struct sock_cmd *last = list;
        for (unsigned i = 1; last && last->control_next && i < BURST_SIZE; i++)
                last = last->control_next;
        owner->control_head = last ? last->control_next : NULL;
        if (last)
                last->control_next = NULL;
        atomic_store_explicit(&owner->control_pending,
                              owner->control_head != NULL,
                              memory_order_release);
        pthread_mutex_unlock(&submit_lock);
        struct sock_cmd *deferred = NULL;
        while (list) {
                struct sock_cmd *cmd = list;
                list = cmd->control_next;
                if (cmd->state == SOCK_CMD_QUEUED) {
                        cmd->control_next = deferred;
                        deferred = cmd;
                } else
                        command_reap(cmd);
        }
        if (deferred) {
                pthread_mutex_lock(&submit_lock);
                struct sock_cmd *tail = deferred;
                while (tail->control_next)
                        tail = tail->control_next;
                tail->control_next = owner->control_head;
                owner->control_head = deferred;
                atomic_store_explicit(&owner->control_pending, true,
                                      memory_order_release);
                pthread_mutex_unlock(&submit_lock);
        }
}

void socket_owner_process_commands(void) {
        struct sock_cmd *commands[BURST_SIZE];
        struct socket_owner *owner = socket_owner_current();
        if (owner == NULL)
                return;
        struct rte_ring *rings[] = {owner->close_ring, owner->command_ring};
        for (unsigned r = 0; r < 2; r++) {
                unsigned count = rte_ring_sc_dequeue_burst(
                    rings[r], (void **)commands, BURST_SIZE, NULL);
                if (count)
                        space_wake(owner);
                for (unsigned i = 0; i < count; i++)
                        command_start(owner, commands[i]);
        }
        command_cancellations(owner);
        command_controls(owner);
}

void socket_owner_shutdown_local(void) {
        struct socket_owner *owner = socket_owner_current();
        if (!owner)
                return;
        pthread_mutex_lock(&submit_lock);
        owner->accepting = false;
        struct sock_cmd *control = owner->control_head;
        owner->control_head = NULL;
        struct sock_cmd *cancellations = owner->cancel_head;
        owner->cancel_head = NULL;
        space_wake(owner);
        pthread_mutex_unlock(&submit_lock);
        struct rte_ring *rings[] = {owner->close_ring, owner->command_ring};
        for (unsigned r = 0; r < 2; r++) {
                struct sock_cmd *cmd;
                while (rte_ring_sc_dequeue(rings[r], (void **)&cmd) == 0) {
                        socket_owner_cancel(cmd, ENETDOWN);
                        command_start(owner, cmd);
                }
        }
        while (owner->active_head) {
                struct sock_cmd *cmd = owner->active_head;
                if (!cmd->done)
                        command_cancel_local(cmd, ENETDOWN);
                /* Shutdown closes all public sockets below, including results
                 * a returning caller has not published yet. */
                command_active_remove(cmd);
                command_put(cmd);
        }
        while (control) {
                struct sock_cmd *next = control->control_next;
                command_put(control);
                control = next;
        }
        while (cancellations) {
                struct sock_cmd *next = cancellations->cancel_next;
                command_put(cancellations);
                cancellations = next;
        }
        for (uint32_t i = 0; i < owner->slot_capacity; i++) {
                struct nsock *sk = owner->slots[i];
                if (!sk || !sk->app_visible)
                        continue;
                sk->app_closed = true;
                socket_public_refresh(sk);
                if (sk->protocol == IPPROTO_TCP)
                        tcp_force_abort(sk, ENETDOWN, "owner-stop");
                else
                        sk->ops->close(sk);
        }
}

int socket_owner_resource_snapshot(struct owner_resource_snapshot *snapshot) {
        struct socket_owner *owner = socket_owner_current();
        struct owner_timer_engine *timer = owner_timer_engine_current();
        struct tcp_memory_snapshot tcp;
        struct udp_memory_snapshot udp;
        _Static_assert((int)TCP_MEMORY_KIND_MAX == (int)OWNER_RESOURCE_udp_rx_node,
                       "TCP resource kinds must match the owner catalog");
        if (snapshot == NULL || owner == NULL || timer == NULL || !timer->initialized) {
                errno = EPERM;
                return -1;
        }
        memset(snapshot, 0, sizeof(*snapshot));
        for (unsigned k = 0; k < TCP_MEMORY_KIND_MAX; k++) {
                if (owner->tcp_memory.pools[k] == NULL) {
                        errno = ENODEV;
                        return -1;
                }
        }
        if (owner->udp_memory.rx_nodes == NULL || owner->ready_event_pool == NULL ||
            owner->free_ids == NULL) {
                errno = ENODEV;
                return -1;
        }
        tcp_owner_memory_snapshot(&owner->tcp_memory, &tcp);
        udp_owner_memory_snapshot(&owner->udp_memory, &udp);
        for (unsigned k = 0; k < TCP_MEMORY_KIND_MAX; k++) {
                struct resource_metric *r = &snapshot->values[k];
                r->capacity = tcp.capacity[k];
                r->current = tcp.capacity[k] - tcp.available[k];
                r->peak = tcp.peak_in_use[k];
                r->unavailable = owner->tcp_memory.unavailable[k];
                r->exhausted = tcp.alloc_fail[k] - r->unavailable;
        }
        snapshot->values[OWNER_RESOURCE_udp_rx_node] = (struct resource_metric){
            .capacity = udp.capacity, .current = udp.capacity - udp.available,
            .peak = udp.peak_in_use, .exhausted = udp.alloc_fail - udp.unavailable,
            .unavailable = udp.unavailable, .limit = udp.queue_drops};
        snapshot->values[OWNER_RESOURCE_socket_slot] = owner->slot_resources;
        snapshot->values[OWNER_RESOURCE_ready_event] = owner->ready_resources;
        snapshot->values[OWNER_RESOURCE_timer] = timer->resources;
        snapshot->values[OWNER_RESOURCE_time_wait] = owner->tcp_memory.time_wait;
        snapshot->values[OWNER_RESOURCE_time_wait].capacity = owner->slot_capacity;
        snapshot->values[OWNER_RESOURCE_tcp_sndbuf_bytes] = owner->tcp_memory.sndbuf_bytes;
        snapshot->values[OWNER_RESOURCE_tcp_unacked_bytes] = owner->tcp_memory.unacked_bytes;
        tcp_ofo_resource_snapshot(&snapshot->values[OWNER_RESOURCE_ofo_segments],
                                  &snapshot->values[OWNER_RESOURCE_ofo_bytes]);
        snapshot->values[OWNER_RESOURCE_ofo_segments].capacity =
            owner->tcp_memory.capacity[TCP_MEMORY_OFO_SEG];
        return 0;
}
