/**
 * @file socket.c
 * @brief Unified socket registry, fd table, and BSD-style API dispatchers.
 *
 * This file owns endpoint indexes, the application fd-to-handle table, common
 * nsock allocation/destruction, and BSD-style command producers.  Public API
 * calls never resolve an fd to a pointer: they copy an @ref nsock_handle and
 * submit a @ref sock_cmd to the packet-worker owner.  Only owner-side packet
 * and command paths use raw nsock pointers.
 */
#include "socket.h"

#include "config.h"
#include "log.h"
#include "net_context.h"
#include "rx_dispatch.h"
#include "tcp.h"

#include "nepoll.h"
#include "socket_bind_internal.h"
#include "socket_owner_internal.h"
#include "socket_public_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_random.h>
#include <rte_ring.h>
#include <rte_timer.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define NSOCK_TX_ARP_BUCKETS 64U

struct local_key {
        uint32_t ip;
        uint16_t port;
        uint16_t pad;
};

struct tcp_conn_key {
        uint32_t remote_ip;
        uint32_t local_ip;
        uint16_t remote_port;
        uint16_t local_port;
};

struct fd_entry {
        bool used, nonblock;
        uint64_t recv_timeout_ns, send_timeout_ns;
        uint32_t readiness;
        struct nsock_handle handle;
};

static struct fd_entry fd_table[NSOCK_FD_MAX];

struct socket_registry {
        struct rte_hash *udp_bind_hash;
        struct rte_hash *tcp_listener_hash;
        struct rte_hash *tcp_conn_hash;
        uint32_t capacity;
        struct nsock *dirty_tx_head;
        struct nsock *dirty_tx_tail;
        struct nsock *arp_wait[NSOCK_TX_ARP_BUCKETS];
        struct nsock_tx_metrics tx_metrics;
        uint32_t dirty_depth;
        uint64_t dirty_budget_exhausted;
        bool ready;
};

static struct socket_registry g_registries[RTE_MAX_LCORE];
static pthread_mutex_t registry_init_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fd_table_lock = PTHREAD_MUTEX_INITIALIZER;
static bool registry_ready;

static struct local_key local_key_make(uint32_t ip, uint16_t port) {
        struct local_key key;
        memset(&key, 0, sizeof(key));
        key.ip = ip;
        key.port = port;
        return key;
}

static struct tcp_conn_key tcp_conn_key_make(const struct nsock *sk) {
        struct tcp_conn_key key;
        memset(&key, 0, sizeof(key));
        key.remote_ip = sk->u.tcp.remote_ip;
        key.local_ip = sk->local_ip;
        key.remote_port = sk->u.tcp.remote_port;
        key.local_port = sk->local_port;
        return key;
}

static struct rte_hash *registry_hash_create(const char *stem,
                                             unsigned int lcore_id,
                                             uint32_t key_len,
                                             uint32_t capacity) {
        char name[RTE_HASH_NAMESIZE];
        const struct rte_hash_parameters p = {
            .name = name,
            .entries = capacity,
            .key_len = key_len,
            .hash_func = rte_jhash,
            .hash_func_init_val = 0,
            .socket_id = rte_socket_id(),
        };

        (void)snprintf(name, sizeof(name), "%s_%u", stem, lcore_id);
        return rte_hash_create(&p);
}

static struct socket_registry *registry_current(void) {
        unsigned int lcore_id = rte_lcore_id();

        if (lcore_id >= RTE_MAX_LCORE || !registry_ready ||
            !g_registries[lcore_id].ready)
                return NULL;
        return &g_registries[lcore_id];
}

int socket_registry_init_owner_with_capacity(unsigned int lcore_id,
                                             uint32_t capacity) {
        struct socket_registry *registry;
        uint32_t hash_capacity;

        if (lcore_id >= RTE_MAX_LCORE || capacity == 0) {
                errno = EINVAL;
                return -1;
        }
        if (capacity > RTE_HASH_ENTRIES_MAX) {
                errno = ERANGE;
                return -1;
        }
        hash_capacity = capacity < NSOCK_REGISTRY_MIN_ENTRIES
                            ? NSOCK_REGISTRY_MIN_ENTRIES
                            : capacity;
        pthread_mutex_lock(&registry_init_lock);
        registry_ready = true;
        registry = &g_registries[lcore_id];
        if (registry->ready) {
                int result = registry->capacity == capacity ? 0 : -1;
                if (result != 0)
                        errno = EBUSY;
                pthread_mutex_unlock(&registry_init_lock);
                return result;
        }

        registry->udp_bind_hash =
            registry_hash_create("nsock_udp_bind", lcore_id,
                                 sizeof(struct local_key), hash_capacity);
        registry->tcp_listener_hash =
            registry_hash_create("nsock_tcp_listener", lcore_id,
                                 sizeof(struct local_key), hash_capacity);
        registry->tcp_conn_hash =
            registry_hash_create("nsock_tcp_conn", lcore_id,
                                 sizeof(struct tcp_conn_key), hash_capacity);

        if (registry->udp_bind_hash == NULL ||
            registry->tcp_listener_hash == NULL ||
            registry->tcp_conn_hash == NULL) {
                if (registry->udp_bind_hash != NULL)
                        rte_hash_free(registry->udp_bind_hash);
                if (registry->tcp_listener_hash != NULL)
                        rte_hash_free(registry->tcp_listener_hash);
                if (registry->tcp_conn_hash != NULL)
                        rte_hash_free(registry->tcp_conn_hash);

                memset(registry, 0, sizeof(*registry));

                pthread_mutex_unlock(&registry_init_lock);
                return -1;
        }

        registry->capacity = capacity;
        registry->ready = true;
        pthread_mutex_unlock(&registry_init_lock);
        return 0;
}

int socket_registry_init_owner(unsigned int lcore_id) {
        return socket_registry_init_owner_with_capacity(
            lcore_id, NSOCK_REGISTRY_DEFAULT_ENTRIES);
}

int socket_registry_init(void) {
        return socket_registry_init_owner(rte_lcore_id());
}

void socket_registry_fini(void) {
        pthread_mutex_lock(&registry_init_lock);
        for (unsigned int lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
                struct socket_registry *registry = &g_registries[lcore_id];

                if (registry->tcp_conn_hash != NULL)
                        rte_hash_free(registry->tcp_conn_hash);
                if (registry->tcp_listener_hash != NULL)
                        rte_hash_free(registry->tcp_listener_hash);
                if (registry->udp_bind_hash != NULL)
                        rte_hash_free(registry->udp_bind_hash);
                memset(registry, 0, sizeof(*registry));
        }
        registry_ready = false;
        pthread_mutex_unlock(&registry_init_lock);

        pthread_mutex_lock(&fd_table_lock);
        memset(fd_table, 0, sizeof(fd_table));
        pthread_mutex_unlock(&fd_table_lock);
}

static int hash_add_unique(struct rte_hash *hash, const void *key,
                           struct nsock *sk) {
        void *old = NULL;
        int rc = rte_hash_lookup_data(hash, key, &old);

        if (rc >= 0) {
                return old == sk ? 0 : -EADDRINUSE;
        }

        rc = rte_hash_add_key_data(hash, key, sk);
        if (rc >= 0)
                return 0;
        return rc == -ENOSPC ? -ENOSPC : -EIO;
}

static void hash_del(struct rte_hash *hash, const void *key) {
        if (hash != NULL)
                (void)rte_hash_del_key(hash, key);
}

int nsock_bind_local(struct nsock *sk, uint32_t ip, uint16_t port) {
        struct socket_registry *registry = registry_current();
        struct rte_hash *hash;
        struct local_key new_key;
        uint8_t flag;
        int rc;

        if (sk == NULL || registry == NULL || port == 0)
                return -EINVAL;

        if (sk->protocol == IPPROTO_UDP) {
                hash = registry->udp_bind_hash;
                flag = NSOCK_REG_UDP_BIND;
        } else if (sk->protocol == IPPROTO_TCP) {
                flag = NSOCK_REG_TCP_BIND;
        } else {
                return -EINVAL;
        }

        new_key = local_key_make(ip, port);

        if (sk->registry_flags & flag)
                return -EINVAL;

        rc = socket_bind_add(sk, ip, port);
        if (rc != 0)
                return rc;
        /* The process-wide binding index owns reservations. TCP connection and
         * listener indexes remain owner-local; UDP routing copies a handle. */
        sk->local_ip = ip;
        sk->local_port = port;
        sk->registry_flags |= flag;
        if (sk->protocol == IPPROTO_UDP && !sk->reuseaddr) {
                rc = hash_add_unique(hash, &new_key, sk);
                if (!rc)
                        rc = rx_dispatch_register_endpoint(
                            sk->protocol, ip, port, sk->owner_lcore);
                if (rc) {
                        hash_del(hash, &new_key);
                        socket_bind_remove(sk);
                        sk->registry_flags &= ~flag;
                        sk->local_ip = 0;
                        sk->local_port = 0;
                        return rc;
                }
        }
        return 0;
}

int nsock_udp_bind_ephemeral(struct nsock *sk, uint32_t ip) {
        static uint16_t next[RTE_MAX_LCORE];
        unsigned int lcore_id = rte_lcore_id();
        const uint32_t port_count =
            UDP_EPHEMERAL_PORT_MAX - UDP_EPHEMERAL_PORT_MIN + 1U;

        if (sk == NULL || sk->protocol != IPPROTO_UDP)
                return -EINVAL;
        if (ip == INADDR_ANY)
                return -EADDRNOTAVAIL;
        if (sk->registry_flags & NSOCK_REG_UDP_BIND)
                return sk->local_ip == ip ? 0 : -EINVAL;
        if (sk->local_ip != 0 || sk->local_port != 0)
                return -EINVAL;
        if (lcore_id >= RTE_MAX_LCORE)
                return -EINVAL;

        if (next[lcore_id] < UDP_EPHEMERAL_PORT_MIN ||
            next[lcore_id] > UDP_EPHEMERAL_PORT_MAX)
                next[lcore_id] =
                    UDP_EPHEMERAL_PORT_MIN + (uint16_t)rte_rand_max(port_count);

        for (uint32_t attempt = 0; attempt < port_count; attempt++) {
                uint16_t host_port = next[lcore_id];
                uint16_t port = htons(host_port);
                int rc;

                next[lcore_id] = host_port == UDP_EPHEMERAL_PORT_MAX
                                     ? UDP_EPHEMERAL_PORT_MIN
                                     : (uint16_t)(host_port + 1U);
                rc = nsock_bind_local(sk, ip, port);
                if (rc == 0)
                        return 0;
                if (rc == -EADDRINUSE || rc == -EEXIST)
                        continue;
                return rc;
        }
        return -EADDRNOTAVAIL;
}

int nsock_tcp_local_taken(uint32_t ip, uint16_t port) {
        return socket_bind_taken(IPPROTO_TCP, ip, port);
}

int nsock_tcp_listener_register(struct nsock *sk) {
        struct socket_registry *registry = registry_current();
        struct local_key key;
        int rc;

        if (sk == NULL || registry == NULL || sk->protocol != IPPROTO_TCP ||
            !(sk->registry_flags & NSOCK_REG_TCP_BIND))
                return -EINVAL;

        key = local_key_make(sk->local_ip, sk->local_port);

        rc = socket_bind_listen(sk, true);
        if (rc != 0)
                return rc;
        rc = hash_add_unique(registry->tcp_listener_hash, &key, sk);
        if (rc != 0) {
                socket_bind_listen(sk, false);
                return rc;
        }
        rc = rx_dispatch_register_endpoint(IPPROTO_TCP, sk->local_ip,
                                           sk->local_port, sk->owner_lcore);
        if (rc != 0) {
                socket_bind_listen(sk, false);
                hash_del(registry->tcp_listener_hash, &key);
                return rc;
        }
        sk->registry_flags |= NSOCK_REG_TCP_LISTENER;
        return 0;
}

void nsock_tcp_listener_unregister(struct nsock *sk) {
        struct socket_registry *registry = registry_current();
        struct local_key key;
        if (sk == NULL || registry == NULL || sk->protocol != IPPROTO_TCP ||
            !(sk->registry_flags & NSOCK_REG_TCP_LISTENER))
                return;

        key = local_key_make(sk->local_ip, sk->local_port);
        hash_del(registry->tcp_listener_hash, &key);
        rx_dispatch_unregister_endpoint(IPPROTO_TCP, sk->local_ip,
                                        sk->local_port, sk->owner_lcore);
        socket_bind_listen(sk, false);
        sk->registry_flags &= (uint8_t)~NSOCK_REG_TCP_LISTENER;
}

int nsock_tcp_conn_register(struct nsock *sk) {
        struct socket_registry *registry = registry_current();
        struct tcp_conn_key key;
        int rc;

        if (sk == NULL || registry == NULL || sk->protocol != IPPROTO_TCP)
                return -EINVAL;

        key = tcp_conn_key_make(sk);

        rc = hash_add_unique(registry->tcp_conn_hash, &key, sk);
        if (rc != 0)
                return rc;
        rc = rx_dispatch_register_tcp_connection(
            sk->u.tcp.remote_ip, sk->local_ip, sk->u.tcp.remote_port,
            sk->local_port, sk->owner_lcore);
        if (rc != 0) {
                hash_del(registry->tcp_conn_hash, &key);
                return rc;
        }
        sk->registry_flags |= NSOCK_REG_TCP_CONN;
        return 0;
}

void nsock_tcp_conn_unregister(struct nsock *sk) {
        struct socket_registry *registry = registry_current();
        struct tcp_conn_key key;
        if (sk == NULL || registry == NULL ||
            !(sk->registry_flags & NSOCK_REG_TCP_CONN))
                return;

        key = tcp_conn_key_make(sk);
        hash_del(registry->tcp_conn_hash, &key);
        rx_dispatch_unregister_tcp_connection(sk->u.tcp.remote_ip, sk->local_ip,
                                              sk->u.tcp.remote_port,
                                              sk->local_port, sk->owner_lcore);
        sk->registry_flags &= (uint8_t)~NSOCK_REG_TCP_CONN;
}

const struct sock_ops *sock_ops_lookup(uint8_t protocol) {
        switch (protocol) {
        case IPPROTO_UDP:
#if ENABLE_UDP_ECHO
                return &udp_ops;
#else
                break;
#endif
        case IPPROTO_TCP:
#if ENABLE_TCP_APP
                return &tcp_ops;
#else
                break;
#endif
        default:
                break;
        }
        return NULL;
}

static uint32_t tx_arp_bucket(uint32_t ip) {
        ip ^= ip >> 16;
        ip ^= ip >> 8;
        return ip & (NSOCK_TX_ARP_BUCKETS - 1U);
}

static void dirty_fifo_append(struct socket_registry *registry,
                              struct nsock *sk, bool requeue) {
        if (sk->tx_dirty_queued || sk->tx_arp_waiting)
                return;

        sk->dirty_prev = registry->dirty_tx_tail;
        sk->dirty_next = NULL;
        if (registry->dirty_tx_tail != NULL)
                registry->dirty_tx_tail->dirty_next = sk;
        else
                registry->dirty_tx_head = sk;
        registry->dirty_tx_tail = sk;
        sk->tx_dirty_queued = true;
        registry->dirty_depth++;
        if (requeue)
                registry->tx_metrics.dirty_requeues++;
        else
                registry->tx_metrics.dirty_enqueues++;
        if (registry->dirty_depth > registry->tx_metrics.dirty_high_water)
                registry->tx_metrics.dirty_high_water = registry->dirty_depth;
}

static void dirty_fifo_unlink(struct socket_registry *registry,
                              struct nsock *sk) {
        if (!sk->tx_dirty_queued)
                return;
        if (sk->dirty_prev != NULL)
                sk->dirty_prev->dirty_next = sk->dirty_next;
        else
                registry->dirty_tx_head = sk->dirty_next;
        if (sk->dirty_next != NULL)
                sk->dirty_next->dirty_prev = sk->dirty_prev;
        else
                registry->dirty_tx_tail = sk->dirty_prev;
        sk->dirty_prev = NULL;
        sk->dirty_next = NULL;
        sk->tx_dirty_queued = false;
        if (registry->dirty_depth > 0)
                registry->dirty_depth--;
}

static struct nsock *dirty_fifo_pop(struct socket_registry *registry) {
        struct nsock *sk = registry->dirty_tx_head;

        if (sk == NULL)
                return NULL;
        dirty_fifo_unlink(registry, sk);
        registry->tx_metrics.dirty_dequeues++;
        return sk;
}

static void arp_wait_link(struct socket_registry *registry, struct nsock *sk) {
        uint32_t bucket = tx_arp_bucket(sk->tx_arp_ip);

        sk->dirty_prev = NULL;
        sk->dirty_next = registry->arp_wait[bucket];
        if (sk->dirty_next != NULL)
                sk->dirty_next->dirty_prev = sk;
        registry->arp_wait[bucket] = sk;
        sk->tx_arp_waiting = true;
        registry->tx_metrics.arp_waits++;
}

static void arp_wait_unlink(struct socket_registry *registry,
                            struct nsock *sk) {
        uint32_t bucket;

        if (!sk->tx_arp_waiting)
                return;
        bucket = tx_arp_bucket(sk->tx_arp_ip);
        if (sk->dirty_prev != NULL)
                sk->dirty_prev->dirty_next = sk->dirty_next;
        else
                registry->arp_wait[bucket] = sk->dirty_next;
        if (sk->dirty_next != NULL)
                sk->dirty_next->dirty_prev = sk->dirty_prev;
        sk->dirty_prev = NULL;
        sk->dirty_next = NULL;
        sk->tx_arp_ip = 0;
        sk->tx_arp_waiting = false;
}

void nsock_tx_mark_dirty(struct nsock *sk) {
        struct socket_registry *registry = registry_current();

        if (sk == NULL || registry == NULL)
                return;
        if (sk->tx_dirty_queued || sk->tx_arp_waiting) {
                registry->tx_metrics.dirty_dedup_hits++;
                return;
        }
        dirty_fifo_append(registry, sk, false);
}

void nsock_tx_dirty_unlink(struct nsock *sk) {
        struct socket_registry *registry = registry_current();

        if (sk == NULL || registry == NULL)
                return;
        if (sk->tx_dirty_queued)
                dirty_fifo_unlink(registry, sk);
        if (sk->tx_arp_waiting)
                arp_wait_unlink(registry, sk);
        sk->tx_dirty_queued = false;
        sk->tx_arp_waiting = false;
        sk->tx_arp_ip = 0;
}

void nsock_tx_arp_wait(struct nsock *sk, uint32_t remote_ip) {
        struct socket_registry *registry = registry_current();

        if (sk == NULL || registry == NULL)
                return;
        if (sk->tx_arp_waiting)
                arp_wait_unlink(registry, sk);
        if (sk->tx_dirty_queued)
                dirty_fifo_unlink(registry, sk);
        sk->tx_arp_ip = remote_ip;
        arp_wait_link(registry, sk);
}

void nsock_tx_arp_resolved(uint32_t remote_ip) {
        struct socket_registry *registry = registry_current();
        uint32_t bucket;
        struct nsock *sk;

        if (registry == NULL)
                return;
        bucket = tx_arp_bucket(remote_ip);
        sk = registry->arp_wait[bucket];
        while (sk != NULL) {
                struct nsock *next = sk->dirty_next;
                if (sk->tx_arp_ip == remote_ip) {
                        arp_wait_unlink(registry, sk);
                        registry->tx_metrics.arp_wakeups++;
                        dirty_fifo_append(registry, sk, false);
                }
                sk = next;
        }
}

void nsock_tx_metrics_take(struct nsock_tx_metrics *out) {
        struct socket_registry *registry;

        if (out == NULL)
                return;
        registry = registry_current();
        if (registry == NULL) {
                memset(out, 0, sizeof(*out));
                return;
        }
        *out = registry->tx_metrics;
        out->dirty_budget_exhausted = registry->dirty_budget_exhausted;
        out->dirty_depth = registry->dirty_depth;
        memset(&registry->tx_metrics, 0, sizeof(registry->tx_metrics));
        registry->dirty_budget_exhausted = 0;
}

void nsock_tx_record_udp_queue_drops(uint64_t count) {
        struct socket_registry *registry = registry_current();

        if (registry != NULL)
                registry->tx_metrics.udp_tx_queue_drops += count;
}

unsigned int nsock_tx_dirty_drain(struct rte_mempool *mp, unsigned int budget) {
        struct socket_registry *registry = registry_current();
        unsigned int flushed = 0;

        if (registry == NULL || budget == 0)
                return 0;
        while (flushed < budget) {
                struct nsock *sk = dirty_fifo_pop(registry);
                int result;

                if (sk == NULL)
                        break;
                result = SOCK_TX_FLUSH_IDLE;
                if (sk->ops != NULL && sk->ops->tx_flush != NULL) {
                        result = sk->ops->tx_flush(sk, mp);
                        registry->tx_metrics.flush_calls++;
                        flushed++;
                }
                if (result == SOCK_TX_FLUSH_DESTROYED)
                        continue;
                /*
                 * A transport may have queued fresh work while flushing
                 * (for example FIN-after-data).  Do not duplicate that entry.
                 * ARP_WAIT has already moved the socket off the hot FIFO.
                 */
                if (!sk->tx_dirty_queued && !sk->tx_arp_waiting &&
                    result == SOCK_TX_FLUSH_RETRY)
                        dirty_fifo_append(registry, sk, true);
        }
        if (flushed == budget && registry->dirty_tx_head != NULL)
                registry->dirty_budget_exhausted++;
        return flushed;
}

/*
 * The fd table deliberately stores generation-checked handles rather than
 * nsock pointers.  Application lcores may copy a handle while holding this
 * lock, but only the packet worker can resolve it to an nsock.  Therefore
 * releasing or reusing a socket slot can never leave an application with a
 * dereferenceable dangling pointer.
 */
static int fd_publish(struct nsock_handle handle, bool nonblock,
                      struct sock_cmd *result) {
        pthread_mutex_lock(&fd_table_lock);
        for (int i = 0; i < NSOCK_FD_MAX; i++) {
                if (!fd_table[i].used) {
                        fd_table[i].used = true;
                        fd_table[i].nonblock = nonblock;
                        fd_table[i].handle = handle;
                        pthread_mutex_unlock(&fd_table_lock);
                        struct sock_cmd cmd = {.type = SOCK_CMD_PUBLISH,
                                               .handle = handle};
                        cmd.args.sockopt.integer = i;
                        int previous_cancel;
                        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
                                               &previous_cancel);
                        if (socket_owner_call(&cmd)) {
                                pthread_mutex_lock(&fd_table_lock);
                                memset(&fd_table[i], 0, sizeof(fd_table[i]));
                                pthread_mutex_unlock(&fd_table_lock);
                                pthread_setcancelstate(previous_cancel, NULL);
                                return -1;
                        }
                        result->published_fd_plus_one = i + 1;
                        socket_owner_result_release(result, true);
                        pthread_setcancelstate(previous_cancel, NULL);
                        return i;
                }
        }
        pthread_mutex_unlock(&fd_table_lock);
        errno = EMFILE;
        return -1;
}

static int fd_resolve(int fd, struct nsock_handle *handle) {
        if (fd < 0 || fd >= NSOCK_FD_MAX || handle == NULL)
                return -EBADF;

        pthread_mutex_lock(&fd_table_lock);
        if (!fd_table[fd].used) {
                pthread_mutex_unlock(&fd_table_lock);
                return -EBADF;
        }
        *handle = fd_table[fd].handle;
        pthread_mutex_unlock(&fd_table_lock);
        return 0;
}

int nsock_tcp_rx_enqueue(struct nsock *sk, struct tcp_rx_blob *blob) {
        if (sk == NULL || blob == NULL)
                return -1;
        if (sk->io_mode == NSOCK_IO_RINGS)
                return rte_ring_sp_enqueue(sk->recv_buf, blob);
        if (sk->u.tcp.rx_queue_count >= RING_SIZE)
                return -1;
        blob->next = NULL;
        if (sk->u.tcp.rx_queue_tail != NULL)
                sk->u.tcp.rx_queue_tail->next = blob;
        else
                sk->u.tcp.rx_queue_head = blob;
        sk->u.tcp.rx_queue_tail = blob;
        sk->u.tcp.rx_queue_count++;
        return 0;
}

struct tcp_rx_blob *nsock_tcp_rx_dequeue(struct nsock *sk) {
        struct tcp_rx_blob *blob;

        if (sk == NULL)
                return NULL;
        if (sk->io_mode == NSOCK_IO_RINGS) {
                if (rte_ring_sc_dequeue(sk->recv_buf, (void **)&blob) != 0)
                        return NULL;
                return blob;
        }
        blob = sk->u.tcp.rx_queue_head;
        if (blob == NULL)
                return NULL;
        sk->u.tcp.rx_queue_head = blob->next;
        if (sk->u.tcp.rx_queue_head == NULL)
                sk->u.tcp.rx_queue_tail = NULL;
        blob->next = NULL;
        sk->u.tcp.rx_queue_count--;
        return blob;
}

uint32_t nsock_tcp_rx_count(const struct nsock *sk) {
        if (sk == NULL)
                return 0;
        if (sk->io_mode == NSOCK_IO_RINGS)
                return rte_ring_count(sk->recv_buf);
        return sk->u.tcp.rx_queue_count;
}

int nsock_tcp_tx_enqueue(struct nsock *sk, struct tcp_fragment *fragment) {
        if (sk == NULL || fragment == NULL)
                return -1;
        if (sk->io_mode == NSOCK_IO_RINGS)
                return rte_ring_sp_enqueue(sk->send_buf, fragment);
        if (sk->u.tcp.tx_queue_count >= RING_SIZE)
                return -1;
        fragment->next = NULL;
        if (sk->u.tcp.tx_queue_tail != NULL)
                sk->u.tcp.tx_queue_tail->next = fragment;
        else
                sk->u.tcp.tx_queue_head = fragment;
        sk->u.tcp.tx_queue_tail = fragment;
        sk->u.tcp.tx_queue_count++;
        return 0;
}

struct tcp_fragment *nsock_tcp_tx_dequeue(struct nsock *sk) {
        struct tcp_fragment *fragment;

        if (sk == NULL)
                return NULL;
        if (sk->io_mode == NSOCK_IO_RINGS) {
                if (rte_ring_sc_dequeue(sk->send_buf, (void **)&fragment) != 0)
                        return NULL;
                return fragment;
        }
        fragment = sk->u.tcp.tx_queue_head;
        if (fragment == NULL)
                return NULL;
        sk->u.tcp.tx_queue_head = fragment->next;
        if (sk->u.tcp.tx_queue_head == NULL)
                sk->u.tcp.tx_queue_tail = NULL;
        fragment->next = NULL;
        sk->u.tcp.tx_queue_count--;
        return fragment;
}

int nsock_tcp_tx_requeue_head(struct nsock *sk, struct tcp_fragment *fragment) {
        if (sk == NULL || fragment == NULL)
                return -1;
        if (sk->io_mode == NSOCK_IO_RINGS) {
                /*
                 * rte_ring has no head-push; preserve the fragment at the cost
                 * of possible reordering with any still-queued segments.
                 */
                return rte_ring_sp_enqueue(sk->send_buf, fragment);
        }
        if (sk->u.tcp.tx_queue_count >= RING_SIZE)
                return -1;
        fragment->next = sk->u.tcp.tx_queue_head;
        sk->u.tcp.tx_queue_head = fragment;
        if (sk->u.tcp.tx_queue_tail == NULL)
                sk->u.tcp.tx_queue_tail = fragment;
        sk->u.tcp.tx_queue_count++;
        return 0;
}

void nsock_set_release_observer(struct nsock *sk, nsock_release_fn fn,
                                void *ctx) {
        if (sk == NULL)
                return;
        sk->release_fn = fn;
        sk->release_ctx = ctx;
}

struct nsock *nsock_alloc_mode(uint8_t protocol, enum nsock_io_mode io_mode) {
        const struct sock_ops *ops = sock_ops_lookup(protocol);
        struct socket_registry *registry = registry_current();

        if (registry == NULL) {
                LOG_ERROR("nsock_alloc: no registry for lcore %u",
                          rte_lcore_id());
                return NULL;
        }
        if (ops == NULL) {
                LOG_ERROR("nsock_alloc: unknown protocol %u", protocol);
                return NULL;
        }
        struct nsock *sk = rte_malloc("nsock", sizeof(struct nsock), 0);
        if (sk == NULL) {
                LOG_ERROR("rte_malloc(nsock) failed");
                return NULL;
        }
        memset(sk, 0, sizeof(*sk));

        sk->id = NSOCK_INVALID_ID;
        sk->protocol = protocol;
        sk->nodelay = true;
        sk->public_fd = -1;
        sk->ops = ops;
        sk->io_mode = io_mode;

        if (io_mode == NSOCK_IO_RINGS) {
                /* Unique ring names so a second socket does not collide. */
                static atomic_uint ring_id = 0;
                unsigned int id = atomic_fetch_add(&ring_id, 1);
                char recv_name[32], send_name[32];
                snprintf(recv_name, sizeof(recv_name), "sock_recv_%u", id);
                snprintf(send_name, sizeof(send_name), "sock_send_%u", id);

                sk->recv_buf =
                    rte_ring_create(recv_name, RING_SIZE, rte_socket_id(),
                                    RING_F_SP_ENQ | RING_F_SC_DEQ);
                sk->send_buf =
                    rte_ring_create(send_name, RING_SIZE, rte_socket_id(),
                                    RING_F_SP_ENQ | RING_F_SC_DEQ);
                if (sk->recv_buf == NULL || sk->send_buf == NULL) {
                        LOG_ERROR("rte_ring_create(nsock) failed");
                        if (sk->recv_buf)
                                rte_ring_free(sk->recv_buf);
                        if (sk->send_buf)
                                rte_ring_free(sk->send_buf);
                        rte_free(sk);
                        return NULL;
                }
        }

        /* TCP starts CLOSED; timer is armed later by connect / RTO paths. */
        if (protocol == IPPROTO_TCP) {
                if (tcp_sndbuf_init(&sk->u.tcp.sndbuf, 0) != 0) {
                        LOG_ERROR("nsock_alloc: tcp_sndbuf_init failed");
                        if (sk->recv_buf != NULL)
                                rte_ring_free(sk->recv_buf);
                        if (sk->send_buf != NULL)
                                rte_ring_free(sk->send_buf);
                        rte_free(sk);
                        return NULL;
                }
                sk->u.tcp.snd_una = 0;
                sk->u.tcp.status = TCP_STATUS_CLOSED;
                tcp_timer_init(sk);
                sk->u.tcp.retries = 0;
                sk->u.tcp.rcvbuf_size = TCP_RCVBUF_SIZE;
                sk->u.tcp.rcvbuf_used = 0;
                sk->u.tcp.rx_current = NULL;
                sk->u.tcp.snd_wnd = 0;
                sk->u.tcp.snd_wl1 = 0;
                sk->u.tcp.snd_wl2 = 0;
                sk->u.tcp.snd_wnd_valid = false;

                sk->u.tcp.snd_mss = TCP_DEFAULT_MSS;
                tcp_sack_state_init(&sk->u.tcp, 0);
                tcp_cc_init_default(&sk->u.tcp, false);

                rb_root_init(&sk->u.tcp.ofo_tree);
                sk->u.tcp.ofo = NULL;
                sk->u.tcp.ofo_tail = NULL;
                sk->u.tcp.ofo_count = 0;
                sk->u.tcp.ofo_bytes = 0;
        }

        rte_memcpy(sk->local_mac, g_net.local_mac, RTE_ETHER_ADDR_LEN);

        return sk;
}

struct nsock *nsock_alloc(uint8_t protocol) {
        return nsock_alloc_mode(protocol, NSOCK_IO_RINGS);
}

void nsock_free(struct nsock *sk) {
        nsock_release_fn release_fn;
        void *release_ctx;
        struct socket_registry *registry;

        if (sk == NULL)
                return;
        if (sk->id != NSOCK_INVALID_ID && rte_lcore_id() != sk->owner_lcore) {
                /*
                 * Failing closed is safer than freeing storage still visible
                 * to its owner.  This should never fire in production; the log
                 * makes any future accidental cross-lcore destructor obvious.
                 */
                LOG_ERROR("reject cross-lcore nsock_free socket=%u owner=%u "
                          "caller=%u",
                          sk->id, sk->owner_lcore, rte_lcore_id());
                return;
        }
        /*
         * Destruction is owner-only.  Retire the generation-checked slot
         * before releasing memory so subsequently dequeued stale commands fail
         * lookup instead of observing a reused allocation.
         */
        socket_owner_retire(sk);
        registry = registry_current();
        if (registry == NULL) {
                LOG_ERROR("reject nsock_free without owner registry socket=%u",
                          sk->id);
                return;
        }

        /* A queued TX pointer must not outlive the socket object. */
        nsock_tx_dirty_unlink(sk);

        /* Drop any pending retransmission/TIME_WAIT callback before free. */
        if (sk->protocol == IPPROTO_TCP) {
                if (sk->u.tcp.status == TCP_STATUS_TIME_WAIT)
                        tcp_stream_set_status(sk, TCP_STATUS_CLOSED);
                tcp_listener_child_detach(sk);
                (void)owner_timer_cancel(&sk->u.tcp.timer);
                tcp_sack_state_reset(&sk->u.tcp, sk->u.tcp.snd_una);
                tcp_sndbuf_free(&sk->u.tcp.sndbuf);
        }
        if (sk->registry_flags & NSOCK_REG_TCP_CONN) {
                struct tcp_conn_key key = tcp_conn_key_make(sk);
                hash_del(registry->tcp_conn_hash, &key);
                rx_dispatch_unregister_tcp_connection(
                    sk->u.tcp.remote_ip, sk->local_ip, sk->u.tcp.remote_port,
                    sk->local_port, sk->owner_lcore);
        }

        if (sk->registry_flags & NSOCK_REG_TCP_LISTENER) {
                struct local_key key =
                    local_key_make(sk->local_ip, sk->local_port);
                hash_del(registry->tcp_listener_hash, &key);
                rx_dispatch_unregister_endpoint(
                    IPPROTO_TCP, sk->local_ip, sk->local_port, sk->owner_lcore);
        }

        if ((sk->registry_flags & NSOCK_REG_UDP_BIND) && !sk->reuseaddr) {
                struct local_key key =
                    local_key_make(sk->local_ip, sk->local_port);
                hash_del(registry->udp_bind_hash, &key);
                rx_dispatch_unregister_endpoint(
                    IPPROTO_UDP, sk->local_ip, sk->local_port, sk->owner_lcore);
        }
        if (sk->registry_flags & (NSOCK_REG_TCP_BIND | NSOCK_REG_UDP_BIND))
                socket_bind_remove(sk);

        sk->registry_flags = 0;
        if (sk->recv_buf != NULL)
                rte_ring_free(sk->recv_buf);
        if (sk->send_buf != NULL)
                rte_ring_free(sk->send_buf);
        release_fn = sk->release_fn;
        release_ctx = sk->release_ctx;
        rte_free(sk);
        if (release_fn != NULL)
                release_fn(release_ctx);
}

struct nsock *nsock_from_ip_port(uint32_t ip, uint16_t port, uint8_t protocol) {
        struct socket_registry *registry = registry_current();
        struct rte_hash *hash;
        struct local_key key;
        struct nsock *sk = NULL;
        void *data = NULL;

        if (registry == NULL)
                return NULL;
        if (protocol == IPPROTO_UDP) {
                struct nsock_handle handle;
                if (socket_bind_udp_shared(port)) {
                        if (!socket_bind_udp_select(ip, port, &handle))
                                return NULL;
                        return socket_owner_resolve_local(handle);
                }
                hash = registry->udp_bind_hash;
        } else if (protocol == IPPROTO_TCP) {
                hash = registry->tcp_listener_hash;
        } else {
                return NULL;
        }

        key = local_key_make(ip, port);

        if (rte_hash_lookup_data(hash, &key, &data) >= 0) {
                sk = (struct nsock *)data;
        }

        if (sk == NULL && ip != INADDR_ANY) {
                key = local_key_make(INADDR_ANY, port);
                if (rte_hash_lookup_data(hash, &key, &data) >= 0) {
                        sk = (struct nsock *)data;
                }
        }
        return sk;
}

struct nsock *nsock_from_4tuple(uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port,
                                uint8_t protocol) {
        struct socket_registry *registry = registry_current();
        struct tcp_conn_key key;
        struct nsock *sk = NULL;
        void *data = NULL;

        if (registry == NULL || protocol != IPPROTO_TCP)
                return NULL;

        memset(&key, 0, sizeof(key));
        key.remote_ip = remote_ip;
        key.local_ip = local_ip;
        key.remote_port = remote_port;
        key.local_port = local_port;

        if (rte_hash_lookup_data(registry->tcp_conn_hash, &key, &data) >= 0)
                sk = (struct nsock *)data;
        return sk;
}

static void result_cleanup(void *arg) {
        struct sock_cmd *descriptor = arg;
        if (descriptor->published_fd_plus_one)
                nclose(descriptor->published_fd_plus_one - 1);
        socket_owner_result_release(descriptor, false);
}

int nsocket(int domain, int type, int protocol) {
        if (domain != AF_INET) {
                errno = EAFNOSUPPORT;
                return -1;
        }
        if (type & ~(SOCK_NONBLOCK | 0xf)) {
                errno = EINVAL;
                return -1;
        }
        int base = type & ~SOCK_NONBLOCK;
        int proto = base == SOCK_STREAM  ? IPPROTO_TCP
                    : base == SOCK_DGRAM ? IPPROTO_UDP
                                         : 0;
        if (!proto) {
                errno = EPROTOTYPE;
                return -1;
        }
        if (protocol && protocol != proto) {
                errno = EPROTONOSUPPORT;
                return -1;
        }
        struct sock_cmd cmd = {.type = SOCK_CMD_CREATE};
        cmd.handle.id = NSOCK_INVALID_ID;
        cmd.args.create.type = type;
        cmd.args.create.protocol = proto;
        int fd = -1, error = 0;
        pthread_cleanup_push(result_cleanup, &cmd);
        if (socket_owner_call(&cmd) == 0) {
                fd = fd_publish(cmd.result_handle, (type & SOCK_NONBLOCK) != 0,
                                &cmd);
                if (fd < 0)
                        error = errno;
                socket_owner_result_release(&cmd, fd >= 0);
        } else
                error = errno;
        pthread_cleanup_pop(0);
        if (fd < 0)
                errno = error;
        return fd;
}

static bool handle_equal(struct nsock_handle a, struct nsock_handle b) {
        return a.id == b.id && a.generation == b.generation &&
               a.owner_lcore == b.owner_lcore && a.protocol == b.protocol;
}

int socket_public_snapshot(int fd, struct nsock_handle *handle,
                           uint32_t *events) {
        if (fd < 0 || fd >= NSOCK_FD_MAX)
                return -1;
        pthread_mutex_lock(&fd_table_lock);
        int rc = fd_table[fd].used ? 0 : -1;
        if (!rc) {
                *handle = fd_table[fd].handle;
                *events = fd_table[fd].readiness;
        }
        pthread_mutex_unlock(&fd_table_lock);
        return rc;
}

void socket_public_refresh(struct nsock *sk) {
        if (!sk || sk->public_fd < 0)
                return;
        uint32_t mask = 0;
        if (sk->protocol == IPPROTO_TCP) {
                if (sk->u.tcp.status == TCP_STATUS_LISTEN) {
                        if (sk->u.tcp.accept_queue &&
                            !rte_ring_empty(sk->u.tcp.accept_queue))
                                mask |= NEPOLL_READ | NEPOLL_ACCEPT;
                } else {
                        if (sk->u.tcp.rx_current || nsock_tcp_rx_count(sk) ||
                            sk->u.tcp.peer_eof)
                                mask |= NEPOLL_READ;
                        if (sk->u.tcp.status == TCP_STATUS_ESTABLISHED ||
                            sk->u.tcp.status == TCP_STATUS_CLOSE_WAIT) {
                                mask |= NEPOLL_CONNECTED;
                                if (tcp_app_snd_space(sk))
                                        mask |= NEPOLL_WRITE;
                        }
                        if (sk->u.tcp.peer_eof || sk->terminal_error)
                                mask |= NEPOLL_HUP | NEPOLL_READ;
                }
        } else {
                if (sk->u.udp.rx_current || sk->u.udp.rx_queue_count ||
                    (sk->recv_buf && !rte_ring_empty(sk->recv_buf)))
                        mask |= NEPOLL_READ;
                if (!sk->send_buf || !rte_ring_full(sk->send_buf))
                        mask |= NEPOLL_WRITE;
        }
        if (sk->pending_error)
                mask |= NEPOLL_ERROR;
        if (sk->app_closed)
                mask = NEPOLL_HUP;
        pthread_mutex_lock(&fd_table_lock);
        struct fd_entry *entry = &fd_table[sk->public_fd];
        bool changed = entry->used &&
                       handle_equal(entry->handle, socket_owner_handle(sk)) &&
                       entry->readiness != mask;
        if (entry->used &&
            handle_equal(entry->handle, socket_owner_handle(sk))) {
                entry->readiness = mask;
                entry->nonblock = sk->nonblock;
                entry->recv_timeout_ns = sk->recv_timeout_ns;
                entry->send_timeout_ns = sk->send_timeout_ns;
        }
        pthread_mutex_unlock(&fd_table_lock);
        if (changed)
                socket_public_notify();
}

static int public_call(int fd, struct sock_cmd *cmd) {
        pthread_mutex_lock(&fd_table_lock);
        if (!fd_table[fd].used ||
            !handle_equal(fd_table[fd].handle, cmd->handle)) {
                pthread_mutex_unlock(&fd_table_lock);
                errno = EBADF;
                return -1;
        }
        cmd->nonblock = fd_table[fd].nonblock;
        bool receive = cmd->type == SOCK_CMD_RECV ||
                       cmd->type == SOCK_CMD_RECVFROM ||
                       cmd->type == SOCK_CMD_ACCEPT;
        if (cmd->type == SOCK_CMD_SEND || cmd->type == SOCK_CMD_RECV ||
            cmd->type == SOCK_CMD_SENDTO || cmd->type == SOCK_CMD_RECVFROM) {
                if (cmd->args.io.flags & ~MSG_DONTWAIT) {
                        pthread_mutex_unlock(&fd_table_lock);
                        errno = EOPNOTSUPP;
                        return -1;
                }
                cmd->nonblock |= (cmd->args.io.flags & MSG_DONTWAIT) != 0;
        }
        if (!cmd->nonblock)
                cmd->timeout_ns = receive ? fd_table[fd].recv_timeout_ns
                                          : fd_table[fd].send_timeout_ns;
        pthread_mutex_unlock(&fd_table_lock);
        int rc = socket_owner_call(cmd);
        if (rc && errno == ETIMEDOUT && cmd->type != SOCK_CMD_CONNECT)
                errno = EAGAIN;
        return rc;
}

int nbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
        struct nsock_handle handle;
        if (addr == NULL || addrlen < sizeof(struct sockaddr_in) ||
            addr->sa_family != AF_INET) {
                errno = EINVAL;
                return -1;
        }
        if (fd_resolve(sockfd, &handle) != 0) {
                errno = EBADF;
                return -1;
        }

        struct sock_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = SOCK_CMD_BIND;
        cmd.handle = handle;
        cmd.args.address.addrlen = addrlen;
        memcpy(&cmd.args.address.addr, addr, sizeof(struct sockaddr_in));

        if (socket_owner_call(&cmd) != 0)
                return -1;
        return 0;
}

ssize_t nsend(int sockfd, const void *buf, size_t len, int flags) {
        struct nsock_handle handle;
        if ((buf == NULL && len != 0) || fd_resolve(sockfd, &handle) != 0) {
                errno = buf == NULL && len != 0 ? EINVAL : EBADF;
                return -1;
        }

        struct sock_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = SOCK_CMD_SEND;
        cmd.handle = handle;
        cmd.args.io.buf = (void *)buf;
        cmd.args.io.len = len;
        cmd.args.io.flags = flags;
        if (public_call(sockfd, &cmd) != 0)
                return -1;
        return cmd.result;
}

ssize_t nrecv(int sockfd, void *buf, size_t len, int flags) {
        struct nsock_handle handle;
        if ((buf == NULL && len != 0) || fd_resolve(sockfd, &handle) != 0) {
                errno = buf == NULL && len != 0 ? EINVAL : EBADF;
                return -1;
        }

        struct sock_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = SOCK_CMD_RECV;
        cmd.handle = handle;
        cmd.args.io.buf = buf;
        cmd.args.io.len = len;
        cmd.args.io.flags = flags;
        if (public_call(sockfd, &cmd) != 0)
                return -1;
        return cmd.result;
}

ssize_t nsendto(int sockfd, const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen) {
        struct nsock_handle handle;
        if ((buf == NULL && len != 0) || dest_addr == NULL ||
            addrlen < sizeof(struct sockaddr_in)) {
                errno = EINVAL;
                return -1;
        }
        if (fd_resolve(sockfd, &handle) != 0) {
                errno = EBADF;
                return -1;
        }

        struct sock_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = SOCK_CMD_SENDTO;
        cmd.handle = handle;
        cmd.args.io.buf = (void *)buf;
        cmd.args.io.len = len;
        cmd.args.io.flags = flags;
        cmd.args.io.addrlen = addrlen;
        memcpy(&cmd.args.io.addr, dest_addr, sizeof(struct sockaddr_in));
        if (public_call(sockfd, &cmd) != 0)
                return -1;
        return cmd.result;
}

ssize_t nrecvfrom(int sockfd, void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, socklen_t *addrlen) {
        if (src_addr && !addrlen) {
                errno = EINVAL;
                return -1;
        }
        struct nsock_handle handle;
        if ((buf == NULL && len != 0) || fd_resolve(sockfd, &handle) != 0) {
                errno = buf == NULL && len != 0 ? EINVAL : EBADF;
                return -1;
        }

        struct sock_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = SOCK_CMD_RECVFROM;
        cmd.handle = handle;
        cmd.args.io.buf = buf;
        cmd.args.io.len = len;
        cmd.args.io.flags = flags;
        cmd.args.io.out_addr = src_addr;
        cmd.args.io.out_addrlen = addrlen;
        if (public_call(sockfd, &cmd) != 0)
                return -1;
        return cmd.result;
}

int nclose(int sockfd) {
        struct nsock_handle handle;
        if (fd_resolve(sockfd, &handle)) {
                errno = EBADF;
                return -1;
        }
        struct sock_cmd cmd = {.type = SOCK_CMD_CLOSE, .handle = handle};
        int old_cancel;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel);
        int rc = socket_owner_call(&cmd);
        int error = errno;
        if (rc == 0 || error == EBADF || error == ENETDOWN) {
                pthread_mutex_lock(&fd_table_lock);
                if (fd_table[sockfd].used &&
                    handle_equal(fd_table[sockfd].handle, handle))
                        memset(&fd_table[sockfd], 0, sizeof(fd_table[sockfd]));
                pthread_mutex_unlock(&fd_table_lock);
                socket_public_notify();
        }
        pthread_setcancelstate(old_cancel, NULL);
        errno = error;
        return rc;
}

static socklen_t option_size(int level, int name) {
        if (level == SOL_SOCKET) {
                if (name == SO_LINGER)
                        return sizeof(struct linger);
                if (name == SO_RCVTIMEO || name == SO_SNDTIMEO)
                        return sizeof(struct timeval);
                if (name == SO_ERROR || name == SO_REUSEADDR)
                        return sizeof(int);
        }
        if (level == IPPROTO_TCP && name == TCP_NODELAY)
                return sizeof(int);
        return 0;
}

int nsetsockopt(int sockfd, int level, int optname, const void *optval,
                socklen_t optlen) {
        struct sock_cmd cmd = {.type = SOCK_CMD_SETSOCKOPT};
        socklen_t size = option_size(level, optname);
        if (!size || (level == SOL_SOCKET && optname == SO_ERROR)) {
                errno = ENOPROTOOPT;
                return -1;
        }
        if (!optval || optlen != size) {
                errno = EINVAL;
                return -1;
        }
        if (fd_resolve(sockfd, &cmd.handle)) {
                errno = EBADF;
                return -1;
        }
        cmd.args.sockopt.level = level;
        cmd.args.sockopt.optname = optname;
        memcpy(&cmd.args.sockopt.value, optval, size);
        if (level == SOL_SOCKET && optname == SO_LINGER &&
            cmd.args.sockopt.value.l_linger < 0) {
                errno = EINVAL;
                return -1;
        }
        if (level == SOL_SOCKET &&
            (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)) {
                struct timeval tv = cmd.args.sockopt.time;
                if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000 ||
                    (uint64_t)tv.tv_sec >
                        (UINT64_MAX - 999999000ULL) / 1000000000ULL) {
                        errno = EINVAL;
                        return -1;
                }
        }
        if (socket_owner_call(&cmd))
                return -1;
        return 0;
}

int ngetsockopt(int sockfd, int level, int optname, void *optval,
                socklen_t *optlen) {
        struct sock_cmd cmd = {.type = SOCK_CMD_GETSOCKOPT};
        socklen_t size = option_size(level, optname);
        if (!size) {
                errno = ENOPROTOOPT;
                return -1;
        }
        if (!optval || !optlen || *optlen < size) {
                errno = EINVAL;
                return -1;
        }
        if (fd_resolve(sockfd, &cmd.handle)) {
                errno = EBADF;
                return -1;
        }
        cmd.args.sockopt.level = level;
        cmd.args.sockopt.optname = optname;
        cmd.args.sockopt.out_value = optval;
        cmd.args.sockopt.out_len = optlen;
        return socket_owner_call(&cmd);
}

int nfcntl(int sockfd, int command, ...) {
        struct sock_cmd cmd = {.type = SOCK_CMD_FLAGS};
        if (command != F_GETFL && command != F_SETFL) {
                errno = EINVAL;
                return -1;
        }
        if (fd_resolve(sockfd, &cmd.handle)) {
                errno = EBADF;
                return -1;
        }
        cmd.args.sockopt.optname = command;
        if (command == F_SETFL) {
                va_list args;
                va_start(args, command);
                cmd.args.sockopt.integer = va_arg(args, int);
                va_end(args);
                if (cmd.args.sockopt.integer & ~(O_NONBLOCK | O_RDWR)) {
                        errno = EINVAL;
                        return -1;
                }
        }
        if (socket_owner_call(&cmd))
                return -1;
        return command == F_GETFL ? (int)cmd.result | O_RDWR : 0;
}

int nconnect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
        struct nsock_handle handle;
        if (addr == NULL || addrlen < sizeof(struct sockaddr_in) ||
            addr->sa_family != AF_INET) {
                errno = EINVAL;
                return -1;
        }
        if (fd_resolve(sockfd, &handle) != 0) {
                errno = EBADF;
                return -1;
        }

        struct sock_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = SOCK_CMD_CONNECT;
        cmd.handle = handle;
        cmd.args.address.addrlen = addrlen;
        memcpy(&cmd.args.address.addr, addr, sizeof(struct sockaddr_in));
        return public_call(sockfd, &cmd);
}

int nlisten(int sockfd, int backlog) {
        struct nsock_handle handle;
        if (fd_resolve(sockfd, &handle) != 0) {
                errno = EBADF;
                return -1;
        }

        struct sock_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = SOCK_CMD_LISTEN;
        cmd.handle = handle;
        cmd.args.listen.backlog = backlog;
        return socket_owner_call(&cmd);
}

int naccept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
        return naccept4(sockfd, addr, addrlen, 0);
}

int naccept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
        if ((flags & ~SOCK_NONBLOCK) || (addr && !addrlen)) {
                errno = EINVAL;
                return -1;
        }
        struct nsock_handle handle;
        if (fd_resolve(sockfd, &handle) != 0) {
                errno = EBADF;
                return -1;
        }

        struct sock_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = SOCK_CMD_ACCEPT;
        cmd.args.address.flags = flags;
        cmd.handle = handle;
        cmd.args.address.out_addr = addr;
        cmd.args.address.out_addrlen = addrlen;
        int child_fd = -1, error = 0;
        pthread_cleanup_push(result_cleanup, &cmd);
        if (public_call(sockfd, &cmd) == 0) {
                child_fd = fd_publish(cmd.result_handle,
                                      (flags & SOCK_NONBLOCK) != 0, &cmd);
                if (child_fd < 0)
                        error = errno;
                socket_owner_result_release(&cmd, child_fd >= 0);
        } else
                error = errno;
        pthread_cleanup_pop(0);
        if (child_fd < 0)
                errno = error;
        return child_fd;
}
