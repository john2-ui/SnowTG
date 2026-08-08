/**
 * @file socket.h
 * @brief Unified userspace socket: one type for UDP and TCP, one registry, and
 *        the BSD-style socket API dispatchers.
 *
 * @ref nsock carries transport-independent state, but is dereferenced only by
 * its packet-worker owner.  Application lcores use integer fds that resolve to
 * generation-checked @ref nsock_handle values and submit @ref sock_cmd
 * requests.  Consequently close/free cannot race an application retaining a
 * raw pointer.
 *
 * Addressing model:
 *   - UDP: socket <-> (local_ip, local_port). Each datagram carries its peer
 *     via @c nsendto / @c nrecvfrom.
 *   - TCP: socket <-> 4-tuple after accept/connect. Use @c nsend / @c nrecv.
 *
 * The socket API entry points are command producers.  Protocol lookup, state
 * mutation, timer processing, and final destruction all run on the owner
 * worker.  The socket list is retained for lifecycle enumeration; TX
 * scheduling is driven by the owner-local dirty TX queue.
 */
#ifndef NETARCH_SOCKET_H
#define NETARCH_SOCKET_H

#include "sock_ops.h"
#include "socket_owner.h"
#include "tcp.h"

#include <pthread.h>
#include <rte_ether.h>
#include <rte_ring.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

/** Highest fd value the in-process fd table can hand out. */
#define NSOCK_FD_MAX 1024
#define NSOCK_REGISTRY_ENTRIES 4096

enum nsock_registry_flags {
        NSOCK_REG_UDP_BIND = 1u << 0,
        NSOCK_REG_TCP_BIND = 1u << 1,
        NSOCK_REG_TCP_LISTENER = 1u << 2,
        NSOCK_REG_TCP_CONN = 1u << 3,
};

/**
 * Queue implementation selected when a socket is allocated.
 *
 * Owner-local sockets never cross an lcore boundary, so their TCP queues can
 * be embedded FIFO lists rather than individually allocated DPDK rings.
 */
enum nsock_io_mode {
        NSOCK_IO_RINGS = 0,
        NSOCK_IO_OWNER_LOCAL,
};

/**
 * @brief One userspace socket, shared by every transport.
 *
 * The worker lcore delivers packets through @c recv_buf; the application lcore
 * consumes @c recv_buf and produces packets through @c send_buf. @c send_buf is
 * opaque to common code: each transport's @ref sock_ops interprets its contents
 * (UDP stores mbufs, TCP stores @ref tcp_fragment).
 */
struct nsock {
        /**
         * Diagnostic fd label only.  Object identity is id+generation; this
         * field is never used for lookup or lifetime ownership.
         */
        int fd;
        uint8_t protocol; /**< IPPROTO_UDP / IPPROTO_TCP. */

        uint32_t local_ip;   /**< Bound IPv4, network byte order. */
        uint16_t local_port; /**< Bound transport port, network byte order. */
        uint8_t local_mac[RTE_ETHER_ADDR_LEN]; /**< Local Ethernet address. */

        struct rte_ring *recv_buf; /**< Worker -> application packet ring. */
        struct rte_ring *send_buf; /**< Application -> worker packet ring. */
        enum nsock_io_mode io_mode;
        nsock_release_fn release_fn;
        void *release_ctx;

        /**
         * Transitional protocol lock/condition.  Application APIs no longer
         * touch these directly; owner conversion lets them be removed once all
         * legacy transport helpers have been simplified.
         */
        pthread_cond_t cond;
        pthread_mutex_t mutex;

        uint32_t id;         /**< Slot in the owner's object table. */
        uint32_t generation; /**< Rejects stale commands after slot reuse. */
        uint16_t
            owner_lcore; /**< Sole lcore allowed to dereference this TCB. */

        /** Socket has been published through CREATE/ACCEPT to an application.
         */
        bool app_visible;
        /** fd has been detached; protocol teardown may still be in progress. */
        bool app_closed;

        /**
         * Owner-local readiness state for traffic-generator reactors.  Event
         * producers OR bits into ready_mask; ready_queued guarantees at most
         * one event-pool entry per live socket.
         */
        uint32_t ready_mask;
        bool ready_queued;

        /** Owner-only queues for blocking BSD operations. */
        struct sock_cmd *recv_wait_head;
        struct sock_cmd *recv_wait_tail;
        struct sock_cmd *send_wait_head;
        struct sock_cmd *send_wait_tail;
        struct sock_cmd *connect_waiter;
        struct sock_cmd *accept_wait_head;
        struct sock_cmd *accept_wait_tail;

        const struct sock_ops *ops; /**< Per-protocol behavior. */

        /** Transport-private state. */
        union {
                struct tcp_stream tcp; /**< TCP connection state. */
        } u;

        uint8_t registry_flags; /**< Flags indicating the socket's registry
                                   state. */

        struct nsock *prev; /**< Previous socket in its owner-local list. */
        struct nsock *next; /**< Next socket in its owner-local list. */

        /*
         * A socket is linked on at most one TX work list at a time:
         * dirty_prev/dirty_next are used by either the dirty FIFO or the
         * ARP-wait bucket selected by tx_arp_ip.
         */
        struct nsock *dirty_prev;
        struct nsock *dirty_next;
        uint32_t tx_arp_ip; /**< Peer address currently blocking TX. */
        bool tx_dirty_queued;
        bool tx_arp_waiting;
};

/**
 * @brief Create the process-wide fd table and this lcore's protocol indexes.
 * @return 0 on success, or -1 when any DPDK hash table cannot be created.
 */
int socket_registry_init(void);
/** @brief Create the protocol indexes owned by @p lcore_id. */
int socket_registry_init_owner(unsigned int lcore_id);
/** @brief Release the process-wide fd table and all owner-local indexes. */
void socket_registry_fini(void);
/** Return the current worker's intrusive socket list, or NULL outside an owner.
 */
struct nsock *nsock_list_local(void);

/**
 * @brief Per-owner TX scheduler counters.
 *
 * These counters are reset by @ref nsock_tx_metrics_take.  The current
 * dirty_depth remains valid after the snapshot so the worker can detect a
 * budget-limited drain.
 */
struct nsock_tx_metrics {
        uint64_t dirty_enqueues;
        uint64_t dirty_dedup_hits;
        uint64_t dirty_dequeues;
        uint64_t dirty_requeues;
        uint64_t flush_calls;
        uint64_t arp_waits;
        uint64_t arp_wakeups;
        uint64_t dirty_budget_exhausted;
        uint32_t dirty_depth;
        uint32_t dirty_high_water;
};

/** Mark an owner-local socket as having immediately runnable TX work. */
void nsock_tx_mark_dirty(struct nsock *sk);
/**
 * Drain at most @p budget dirty sockets.  The transport return value controls
 * whether a socket is requeued, waits for ARP, or becomes idle.
 */
unsigned int nsock_tx_dirty_drain(struct rte_mempool *mp, unsigned int budget);
/** Remove a socket from the dirty FIFO or ARP-wait bucket before freeing it. */
void nsock_tx_dirty_unlink(struct nsock *sk);
/**
 * Move a socket whose head packet needs neighbour resolution to an ARP-wait
 * bucket.  The socket is woken when that IPv4 address becomes usable.
 */
void nsock_tx_arp_wait(struct nsock *sk, uint32_t remote_ip);
/** Wake sockets waiting for @p remote_ip after ARP learning or expiry retry. */
void nsock_tx_arp_resolved(uint32_t remote_ip);
/** Snapshot and clear per-owner dirty-TX counters. */
void nsock_tx_metrics_take(struct nsock_tx_metrics *out);

/**
 * @brief Bind a socket to a local endpoint and reserve it in its protocol map.
 * @return 0 on success, or a negative errno-style error code.
 */
int nsock_bind_local(struct nsock *sk, uint32_t ip, uint16_t port);
/** @brief Publish a bound TCP socket as a listener endpoint. */
int nsock_tcp_listener_register(struct nsock *sk);
/** @brief Remove a TCP listener endpoint from the listener index. */
void nsock_tcp_listener_unregister(struct nsock *sk);
/** @brief Publish a TCP TCB in the exact four-tuple connection index. */
int nsock_tcp_conn_register(struct nsock *sk);
/** @brief Remove a TCP TCB from the exact four-tuple connection index. */
void nsock_tcp_conn_unregister(struct nsock *sk);
/** @brief Return non-zero when a TCP local endpoint is already reserved. */
int nsock_tcp_local_taken(uint32_t ip, uint16_t port);

/**
 * @name socket allocation
 * @{
 */
/**
 * @brief Allocate and register a @ref nsock with fresh, uniquely-named rings.
 *
 * @param fd       Optional diagnostic label; pass -1 for owner-created
 *                 sockets.  It does not participate in object lookup.
 * @param protocol IP protocol number; selects the @ref sock_ops to bind.
 * @return Initialized socket, or NULL on allocation failure.
 */
struct nsock *nsock_alloc(int fd, uint8_t protocol);
/** Allocate a socket with an explicit queue implementation. */
struct nsock *nsock_alloc_mode(int fd, uint8_t protocol,
                               enum nsock_io_mode io_mode);
/** Install a one-shot final-release observer owned by the packet worker. */
void nsock_set_release_observer(struct nsock *sk, nsock_release_fn fn,
                                void *ctx);

/** TCP queue helpers that select the socket's configured I/O mode. */
int nsock_tcp_rx_enqueue(struct nsock *sk, struct tcp_rx_blob *blob);
struct tcp_rx_blob *nsock_tcp_rx_dequeue(struct nsock *sk);
uint32_t nsock_tcp_rx_count(const struct nsock *sk);
int nsock_tcp_tx_enqueue(struct nsock *sk, struct tcp_fragment *fragment);
struct tcp_fragment *nsock_tcp_tx_dequeue(struct nsock *sk);
/**
 * Put a just-dequeued control fragment back at the TX queue head so later
 * segments keep their relative order across ARP/mempool retries.
 */
int nsock_tcp_tx_requeue_head(struct nsock *sk, struct tcp_fragment *fragment);
/**
 * Retire @p sk from its owner and indexes, then release all object storage.
 * Must be called by the owning packet worker.
 */
void nsock_free(struct nsock *sk);
/** @} */

/**
 * @name registry lookups
 * @{
 */
/*
 * Packet-path lookups return raw pointers safely because both the indexes and
 * returned objects are owned and consumed by the same worker lcore.
 */
struct nsock *nsock_from_ip_port(uint32_t ip, uint16_t port, uint8_t protocol);
struct nsock *nsock_from_4tuple(uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port,
                                uint8_t protocol);

/** @} */

/**
 * @name BSD-style socket API (dispatchers)
 * @{
 */
int nsocket(int domain, int type, int protocol);
int nbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
/** Connected send (TCP). */
ssize_t nsend(int sockfd, const void *buf, size_t len, int flags);
/** Connected receive (TCP). */
ssize_t nrecv(int sockfd, void *buf, size_t len, int flags);
/** Datagram send (UDP); @p dest_addr is the peer for this packet. */
ssize_t nsendto(int sockfd, const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen);
/** Datagram receive (UDP); @p src_addr receives the peer of this packet. */
ssize_t nrecvfrom(int sockfd, void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, socklen_t *addrlen);
int nclose(int sockfd);
int nconnect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int nlisten(int sockfd, int backlog);
int naccept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
/** @} */

#endif /* NETARCH_SOCKET_H */
