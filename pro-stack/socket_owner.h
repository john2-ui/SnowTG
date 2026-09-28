/**
 * @file socket_owner.h
 * @brief Cross-lcore socket command channel and generation-checked handles.
 *
 * A transport control block is mutable state.  Letting application lcores,
 * the packet worker, and timer callbacks dereference the same @ref nsock made
 * close/free race with send, receive, lookup, and timer expiry.  The owner
 * model removes that class of race instead of trying to cover every field
 * with another lock:
 *
 *   - the packet worker is the only lcore allowed to dereference an nsock;
 *   - applications retain an opaque (slot, generation, owner) handle;
 *   - BSD-style API calls submit commands and wait for completion;
 *   - blocking operations are parked on owner-only wait queues, so the packet
 *     worker never blocks and can continue receiving ACKs/data.
 *
 * The generation is incremented whenever a slot is retired.  A delayed
 * command carrying an old generation therefore cannot accidentally operate on
 * a new socket that reused the same slot (the classic ABA problem).
 */
#ifndef NETARCH_SOCKET_OWNER_H
#define NETARCH_SOCKET_OWNER_H

#include "owner_timer.h"
#include "tcp_memory.h"
#include "udp_memory.h"
#include <stdatomic.h>
#include <sys/time.h>

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

/** Default number of TCB slots owned by each packet worker. */
#define NSOCK_ID_DEFAULT_CAPACITY 4096U
/**
 * Compatibility name for callers that used the old compile-time default.
 *
 * This is no longer a runtime upper bound.  Owner contexts created with the
 * capacity-aware initializer may use a different slot count.
 */
#define NSOCK_ID_MAX NSOCK_ID_DEFAULT_CAPACITY
/** Sentinel used before a command has produced a socket handle. */
#define NSOCK_INVALID_ID UINT32_MAX

struct nsock;
struct rte_ring;
struct rte_mempool;
/** Called after an owner-local socket has released all resources. */
typedef void (*nsock_release_fn)(void *ctx);

/** Stable cross-lcore name for a socket; it never contains a raw pointer. */
struct nsock_handle {
        uint32_t id;
        uint32_t generation;
        uint16_t owner_lcore;
        uint8_t protocol;
};

enum sock_cmd_type {
        SOCK_CMD_CREATE,
        SOCK_CMD_BIND,
        SOCK_CMD_CONNECT,
        SOCK_CMD_LISTEN,
        SOCK_CMD_ACCEPT,
        SOCK_CMD_SEND,
        SOCK_CMD_RECV,
        SOCK_CMD_SENDTO,
        SOCK_CMD_RECVFROM,
        SOCK_CMD_SETSOCKOPT,
        SOCK_CMD_GETSOCKOPT,
        SOCK_CMD_CLOSE,
        SOCK_CMD_FLAGS,
        SOCK_CMD_PUBLISH,
};

/** Request descriptor. socket_owner_call clones this into independently owned
 * storage; only the clone is queued, and no caller buffer reaches the owner. */
enum sock_cmd_state {
        SOCK_CMD_NEW,
        SOCK_CMD_QUEUED,
        SOCK_CMD_RUNNING,
        SOCK_CMD_PARKED,
        SOCK_CMD_DONE,
        SOCK_CMD_CANCELLED
};
struct sock_cmd {
        enum sock_cmd_type type;
        struct nsock_handle handle;

        union {
                struct {
                        int type;
                        int protocol;
                } create;

                struct {
                        /** Input address copied by value before enqueue. */
                        struct sockaddr_storage addr;
                        socklen_t addrlen;
                        /** Descriptor output pointer; the queued clone
                         * substitutes owned storage. */
                        struct sockaddr *out_addr;
                        socklen_t *out_addrlen;
                        int flags;
                } address;

                struct {
                        void *buf;
                        size_t len;
                        int flags;
                        /** sendto destination, copied by value. */
                        struct sockaddr_storage addr;
                        socklen_t addrlen;
                        /** Descriptor output pointers, replaced before enqueue.
                         */
                        struct sockaddr *out_addr;
                        socklen_t *out_addrlen;
                } io;

                struct {
                        int backlog;
                } listen;

                struct {
                        int level;
                        int optname;
                        union {
                                struct linger value;
                                struct timeval time;
                                int integer;
                        };
                        void *out_value;
                        socklen_t *out_len;
                } sockopt;
        } args;

        /** Handle returned by CREATE or ACCEPT before an fd is published. */
        struct nsock_handle result_handle;

        ssize_t result;
        int error;

        pthread_mutex_t done_mutex;
        pthread_cond_t done_cond;
        bool done;

        /* Managed clone state. The descriptor supplied by the API has none
         * of these resources initialized. */
        atomic_uint refs;
        atomic_int cancel_error;
        enum sock_cmd_state state;
        bool claimed;
        bool nonblock;
        uint64_t timeout_ns;
        uint64_t deadline_ns;
        struct socket_owner *owner;
        struct owner_timer timer;
        void *storage;
        struct sockaddr_storage output_addr;
        socklen_t output_len;
        struct sock_cmd *active_prev, *active_next, *control_next;
        struct sock_cmd
            *cancel_next; /**< One notification/reference per request. */
        struct sock_cmd *managed_result;
        int published_fd_plus_one;

        /** Owner-only linkage for recv/send/accept wait queues. */
        struct sock_cmd *next;
};

/** Per-worker object table and MPSC application command queue. */
struct socket_owner {
        unsigned int lcore_id;
        struct rte_ring *command_ring;
        struct rte_ring *close_ring;
        atomic_uint space_seq;
        bool accepting; /* protected by submission lock */
        atomic_bool cancel_pending;
        struct sock_cmd
            *cancel_head; /**< Submission lock; cancellation owns a ref. */
        atomic_bool control_pending; /**< Empty fast path never takes submission
                                        lock. */
        struct sock_cmd
            *control_head; /* submission lock; intrusive, no allocation */
        struct sock_cmd *active_head; /* owner only */
        uint64_t command_waits, command_cancels, command_peak;
        bool ready_overflow;
        uint32_t ready_scan;
        uint64_t ready_recoveries;
        /** Owner-local, coalesced transport readiness notifications. */
        struct rte_ring *ready_ring;
        /** Preallocated event objects; at most one is queued per socket. */
        struct rte_mempool *ready_event_pool;
        /** Owner-local TCP hot-path pools; copied with each future shard. */
        struct tcp_owner_memory tcp_memory;
        /** Owner-local UDP receive-queue metadata pool. */
        struct udp_owner_memory udp_memory;

        /** Runtime-sized socket slot table and generation array. */
        struct nsock **slots;
        uint32_t *generations;
        /** LIFO of currently unused slot IDs for O(1) allocation. */
        uint32_t *free_ids;
        uint32_t slot_capacity;
        uint32_t free_count;
        uint32_t ready_capacity;
        struct resource_metric slot_resources, ready_resources;
};

/** Initialize the context for one packet-worker lcore. */
int socket_owner_init(unsigned int lcore_id);
/**
 * Initialize one packet-worker owner with an explicit slot capacity.
 *
 * The capacity is fixed until @ref socket_owner_fini; runtime resizing is not
 * supported because the owner queues and protocol indexes are initialized
 * alongside the slot table.
 */
int socket_owner_init_with_capacity(unsigned int lcore_id, uint32_t capacity);
/** Release every initialized owner context after all workers have stopped. */
void socket_owner_fini(void);
/**
 * @brief Deep-copy a descriptor, submit it, and wait for owner completion.
 * Input/output pointers are used only on the caller thread. For successful
 * CREATE/ACCEPT the caller must release the managed result after publication,
 * including via a pthread cleanup handler if publication is interrupted.
 */
int socket_owner_call(struct sock_cmd *cmd);
/** Drain a burst of commands; called only from the packet worker. */
void socket_owner_process_commands(void);
void socket_owner_complete(struct sock_cmd *cmd, ssize_t result, int error);
/** Thread-safe cancellation of a managed clone; retain a reference throughout
 * the call. The first reason wins. A coalesced notification owns an extra ref
 * until the owner observes it, even if the caller continues waiting.
 */
void socket_owner_cancel(struct sock_cmd *cmd, int error);
/** Settle CREATE/ACCEPT publication; unclaimed sockets are closed by the owner.
 */
void socket_owner_result_release(struct sock_cmd *descriptor, bool claimed);
/** Stop admission and reclaim outstanding commands on the owning worker. */
void socket_owner_shutdown_local(void);
/** Process-wide live managed requests, including returned but unreaped results.
 */
uint64_t socket_owner_command_live(void);

/** Register a newly allocated socket in the current owner's slot table. */
int socket_owner_adopt(struct nsock *sk);
/** Remove a socket from the slot table immediately before final destruction. */
void socket_owner_retire(struct nsock *sk);
/** Construct a handle for an already adopted socket. */
struct nsock_handle socket_owner_handle(const struct nsock *sk);

/** Retry owner-parked operations after protocol state made progress. */
void socket_owner_wake_recv(struct nsock *sk);
void socket_owner_wake_send(struct nsock *sk);
void socket_owner_wake_accept(struct nsock *listener);
void socket_owner_complete_connect(struct nsock *sk, int error);

/** Fail every application waiter during reset, close, or destruction. */
void socket_owner_abort_waiters(struct nsock *sk, int error);

#endif /* NETARCH_SOCKET_OWNER_H */
