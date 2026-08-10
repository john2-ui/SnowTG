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

#include "tcp_memory.h"
#include "udp_memory.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

/** Number of TCB slots owned by each packet worker. */
#define NSOCK_ID_MAX 4096U
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
        SOCK_CMD_CLOSE,
};

/**
 * @brief One synchronous application request.
 *
 * The caller owns this object (currently on its stack) and waits on
 * @c done_cond until the owner completes it.  A blocking recv/connect/accept
 * may remain linked in an nsock wait queue for an arbitrary time; the caller
 * consequently must not return or destroy the command before completion.
 */
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
                        /** ACCEPT output storage; valid while caller waits. */
                        struct sockaddr *out_addr;
                        socklen_t *out_addrlen;
                } address;

                struct {
                        void *buf;
                        size_t len;
                        int flags;
                        /** sendto destination, copied by value. */
                        struct sockaddr_storage addr;
                        socklen_t addrlen;
                        /** recvfrom output storage; caller blocks while used.
                         */
                        struct sockaddr *out_addr;
                        socklen_t *out_addrlen;
                } io;

                struct {
                        int backlog;
                } listen;
        } args;

        /** Handle returned by CREATE or ACCEPT before an fd is published. */
        struct nsock_handle result_handle;

        ssize_t result;
        int error;

        pthread_mutex_t done_mutex;
        pthread_cond_t done_cond;
        bool done;

        /** Owner-only linkage for recv/send/accept wait queues. */
        struct sock_cmd *next;
};

/** Per-worker object table and MPSC application command queue. */
struct socket_owner {
        unsigned int lcore_id;
        struct rte_ring *command_ring;
        /** Owner-local, coalesced transport readiness notifications. */
        struct rte_ring *ready_ring;
        /** Preallocated event objects; at most one is queued per socket. */
        struct rte_mempool *ready_event_pool;
        /** Owner-local TCP hot-path pools; copied with each future shard. */
        struct tcp_owner_memory tcp_memory;
        /** Owner-local UDP receive-queue metadata pool. */
        struct udp_owner_memory udp_memory;

        struct nsock *slots[NSOCK_ID_MAX];
        uint32_t generations[NSOCK_ID_MAX];
};

/** Initialize the context for one packet-worker lcore. */
int socket_owner_init(unsigned int lcore_id);
/** Release every initialized owner context after all workers have stopped. */
void socket_owner_fini(void);
/** Submit a command to its owner and wait until it is completed. */
int socket_owner_call(struct sock_cmd *cmd);
/** Drain a burst of commands; called only from the packet worker. */
void socket_owner_process_commands(void);
void socket_owner_complete(struct sock_cmd *cmd, ssize_t result, int error);

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