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
#include "list.h"
#include "log.h"
#include "net_context.h"
#include "tcp.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_ring.h>
#include <rte_timer.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

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
        bool used;
        struct nsock_handle handle;
};

static struct fd_entry fd_table[NSOCK_FD_MAX];

struct socket_registry {
        struct rte_hash *udp_bind_hash;
        struct rte_hash *tcp_bind_hash;
        struct rte_hash *tcp_listener_hash;
        struct rte_hash *tcp_conn_hash;
        struct nsock *sock_list;
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
                                             uint32_t key_len) {
        char name[RTE_HASH_NAMESIZE];
        const struct rte_hash_parameters p = {
            .name = name,
            .entries = NSOCK_REGISTRY_ENTRIES,
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

int socket_registry_init_owner(unsigned int lcore_id) {
        struct socket_registry *registry;

        if (lcore_id >= RTE_MAX_LCORE)
                return -1;
        pthread_mutex_lock(&registry_init_lock);
        registry_ready = true;
        registry = &g_registries[lcore_id];
        if (registry->ready) {
                pthread_mutex_unlock(&registry_init_lock);
                return 0;
        }

        registry->udp_bind_hash = registry_hash_create(
            "nsock_udp_bind", lcore_id, sizeof(struct local_key));
        registry->tcp_bind_hash = registry_hash_create(
            "nsock_tcp_bind", lcore_id, sizeof(struct local_key));
        registry->tcp_listener_hash = registry_hash_create(
            "nsock_tcp_listener", lcore_id, sizeof(struct local_key));
        registry->tcp_conn_hash = registry_hash_create(
            "nsock_tcp_conn", lcore_id, sizeof(struct tcp_conn_key));

        if (registry->udp_bind_hash == NULL || registry->tcp_bind_hash == NULL ||
            registry->tcp_listener_hash == NULL ||
            registry->tcp_conn_hash == NULL) {
                if (registry->udp_bind_hash != NULL)
                        rte_hash_free(registry->udp_bind_hash);
                if (registry->tcp_bind_hash != NULL)
                        rte_hash_free(registry->tcp_bind_hash);
                if (registry->tcp_listener_hash != NULL)
                        rte_hash_free(registry->tcp_listener_hash);
                if (registry->tcp_conn_hash != NULL)
                        rte_hash_free(registry->tcp_conn_hash);

                memset(registry, 0, sizeof(*registry));

                pthread_mutex_unlock(&registry_init_lock);
                return -1;
        }

        registry->ready = true;
        pthread_mutex_unlock(&registry_init_lock);
        return 0;
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
                if (registry->tcp_bind_hash != NULL)
                        rte_hash_free(registry->tcp_bind_hash);
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
        return rc < 0 ? -1 : 0;
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
                hash = registry->tcp_bind_hash;
                flag = NSOCK_REG_TCP_BIND;
        } else {
                return -EINVAL;
        }

        new_key = local_key_make(ip, port);

        if (sk->registry_flags & flag)
                return -EINVAL;

        rc = hash_add_unique(hash, &new_key, sk);
        if (rc == 0) {
                sk->local_ip = ip;
                sk->local_port = port;
                sk->registry_flags |= flag;
        }

        return rc;
}

int nsock_tcp_local_taken(uint32_t ip, uint16_t port) {
        struct socket_registry *registry = registry_current();
        struct local_key key = local_key_make(ip, port);
        void *data = NULL;

        return registry != NULL &&
               rte_hash_lookup_data(registry->tcp_bind_hash, &key, &data) >= 0;
}

int nsock_tcp_listener_register(struct nsock *sk) {
        struct socket_registry *registry = registry_current();
        struct local_key key;
        int rc;

        if (sk == NULL || registry == NULL || sk->protocol != IPPROTO_TCP ||
            !(sk->registry_flags & NSOCK_REG_TCP_BIND))
                return -EINVAL;

        key = local_key_make(sk->local_ip, sk->local_port);

        rc = hash_add_unique(registry->tcp_listener_hash, &key, sk);
        if (rc == 0)
                sk->registry_flags |= NSOCK_REG_TCP_LISTENER;
        return rc;
}

void nsock_tcp_listener_unregister(struct nsock *sk) {
        struct socket_registry *registry = registry_current();
        struct local_key key;
        if (sk == NULL || registry == NULL || sk->protocol != IPPROTO_TCP ||
            !(sk->registry_flags & NSOCK_REG_TCP_LISTENER))
                return;

        key = local_key_make(sk->local_ip, sk->local_port);
        hash_del(registry->tcp_listener_hash, &key);
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
        if (rc == 0)
                sk->registry_flags |= NSOCK_REG_TCP_CONN;
        return rc;
}

void nsock_tcp_conn_unregister(struct nsock *sk) {
        struct socket_registry *registry = registry_current();
        struct tcp_conn_key key;
        if (sk == NULL || registry == NULL ||
            !(sk->registry_flags & NSOCK_REG_TCP_CONN))
                return;

        key = tcp_conn_key_make(sk);
        hash_del(registry->tcp_conn_hash, &key);
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

struct nsock *nsock_list_local(void) {
        struct socket_registry *registry = registry_current();

        return registry == NULL ? NULL : registry->sock_list;
}

/*
 * The fd table deliberately stores generation-checked handles rather than
 * nsock pointers.  Application lcores may copy a handle while holding this
 * lock, but only the packet worker can resolve it to an nsock.  Therefore
 * releasing or reusing a socket slot can never leave an application with a
 * dereferenceable dangling pointer.
 */
static int fd_publish(struct nsock_handle handle) {
        pthread_mutex_lock(&fd_table_lock);
        for (int i = 0; i < NSOCK_FD_MAX; i++) {
                if (!fd_table[i].used) {
                        fd_table[i].used = true;
                        fd_table[i].handle = handle;
                        pthread_mutex_unlock(&fd_table_lock);
                        return i;
                }
        }
        pthread_mutex_unlock(&fd_table_lock);
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

/*
 * Atomically remove an fd and return its former handle.  This is the
 * linearization point for close: after fd_take succeeds every later API call
 * observes EBADF, although the owner may retain the TCP TCB through FIN and
 * TIME_WAIT.
 */
static int fd_take(int fd, struct nsock_handle *handle) {
        if (fd < 0 || fd >= NSOCK_FD_MAX || handle == NULL)
                return -EBADF;

        pthread_mutex_lock(&fd_table_lock);
        if (!fd_table[fd].used) {
                pthread_mutex_unlock(&fd_table_lock);
                return -EBADF;
        }
        *handle = fd_table[fd].handle;
        memset(&fd_table[fd], 0, sizeof(fd_table[fd]));
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
                return rte_ring_mp_enqueue(sk->send_buf, fragment);
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

void nsock_set_release_observer(struct nsock *sk, nsock_release_fn fn,
                                void *ctx) {
        if (sk == NULL)
                return;
        sk->release_fn = fn;
        sk->release_ctx = ctx;
}

struct nsock *nsock_alloc_mode(int fd, uint8_t protocol,
                               enum nsock_io_mode io_mode) {
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

        /*
         * fd is retained only as a diagnostic label during migration.  It is
         * never used to find or own this object; fd_table stores a handle.
         */
        sk->fd = fd;
        sk->id = NSOCK_INVALID_ID;
        sk->protocol = protocol;
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
                sk->send_buf = rte_ring_create(send_name, RING_SIZE,
                                               rte_socket_id(), RING_F_SC_DEQ);
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

        if (pthread_mutex_init(&sk->mutex, NULL) != 0 ||
            pthread_cond_init(&sk->cond, NULL) != 0) {
                LOG_ERROR("pthread init failed");
                if (sk->recv_buf != NULL)
                        rte_ring_free(sk->recv_buf);
                if (sk->send_buf != NULL)
                        rte_ring_free(sk->send_buf);
                rte_free(sk);
                return NULL;
        }

        /* TCP starts CLOSED; timer is armed later by connect / RTO paths. */
        if (protocol == IPPROTO_TCP) {
                if (tcp_sndbuf_init(&sk->u.tcp.sndbuf, 0) != 0) {
                        LOG_ERROR("nsock_alloc: tcp_sndbuf_init failed");
                        if (sk->recv_buf != NULL)
                                rte_ring_free(sk->recv_buf);
                        if (sk->send_buf != NULL)
                                rte_ring_free(sk->send_buf);
                        pthread_mutex_destroy(&sk->mutex);
                        pthread_cond_destroy(&sk->cond);
                        rte_free(sk);
                        return NULL;
                }
                sk->u.tcp.snd_una = 0;
                sk->u.tcp.status = TCP_STATUS_CLOSED;
                rte_timer_init(&sk->u.tcp.timer);
                sk->u.tcp.retries = 0;
                sk->u.tcp.rcvbuf_size = TCP_RCVBUF_SIZE;
                sk->u.tcp.rcvbuf_used = 0;
                sk->u.tcp.rx_current = NULL;
                sk->u.tcp.snd_wnd = 0;
                sk->u.tcp.snd_wl1 = 0;
                sk->u.tcp.snd_wl2 = 0;
                sk->u.tcp.snd_wnd_valid = false;

                sk->u.tcp.snd_mss = TCP_DEFAULT_MSS;

                rb_root_init(&sk->u.tcp.ofo_tree);
                sk->u.tcp.ofo = NULL;
                sk->u.tcp.ofo_tail = NULL;
                sk->u.tcp.ofo_count = 0;
                sk->u.tcp.ofo_bytes = 0;
        }

        rte_memcpy(sk->local_mac, g_net.local_mac, RTE_ETHER_ADDR_LEN);

        LL_ADD(sk, registry->sock_list);
        return sk;
}

struct nsock *nsock_alloc(int fd, uint8_t protocol) {
        return nsock_alloc_mode(fd, protocol, NSOCK_IO_RINGS);
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

        /* Drop any pending retransmission/TIME_WAIT callback before free. */
        if (sk->protocol == IPPROTO_TCP) {
                rte_timer_stop(&sk->u.tcp.timer);
                tcp_sndbuf_free(&sk->u.tcp.sndbuf);
        }
        if (sk->registry_flags & NSOCK_REG_TCP_CONN) {
                struct tcp_conn_key key = tcp_conn_key_make(sk);
                hash_del(registry->tcp_conn_hash, &key);
        }

        if (sk->registry_flags & NSOCK_REG_TCP_LISTENER) {
                struct local_key key =
                    local_key_make(sk->local_ip, sk->local_port);
                hash_del(registry->tcp_listener_hash, &key);
        }

        if (sk->registry_flags & NSOCK_REG_TCP_BIND) {
                struct local_key key =
                    local_key_make(sk->local_ip, sk->local_port);
                hash_del(registry->tcp_bind_hash, &key);
        }

        if (sk->registry_flags & NSOCK_REG_UDP_BIND) {
                struct local_key key =
                    local_key_make(sk->local_ip, sk->local_port);
                hash_del(registry->udp_bind_hash, &key);
        }

        LL_REMOVE(sk, registry->sock_list);
        sk->registry_flags = 0;
        pthread_cond_destroy(&sk->cond);
        pthread_mutex_destroy(&sk->mutex);
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

int nsocket(__attribute__((unused)) int domain, int type,
            __attribute__((unused)) int protocol) {
        uint8_t proto;
        switch (type) {
        case SOCK_DGRAM:
                proto = IPPROTO_UDP;
                break;
        case SOCK_STREAM:
                proto = IPPROTO_TCP;
                break;
        default:
                LOG_ERROR("nsocket: unsupported socket type %d", type);
                return -1;
        }

        struct sock_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = SOCK_CMD_CREATE;
        cmd.handle.id = NSOCK_INVALID_ID;
        cmd.args.create.type = type;
        cmd.args.create.protocol = proto;

        if (socket_owner_call(&cmd) != 0) {
                LOG_ERROR("nsocket: owner failed to create proto=%u", proto);
                return -1;
        }

        int fd = fd_publish(cmd.result_handle);
        if (fd < 0) {
                LOG_ERROR("nsocket: fd table full");
                /*
                 * The object already exists on the owner.  Close it through
                 * the same command path so no raw pointer or orphan escapes.
                 */
                struct sock_cmd close_cmd;
                memset(&close_cmd, 0, sizeof(close_cmd));
                close_cmd.type = SOCK_CMD_CLOSE;
                close_cmd.handle = cmd.result_handle;
                (void)socket_owner_call(&close_cmd);
                errno = EMFILE;
                return -1;
        }

        LOG_INFO("nsocket type=%d proto=%u fd=%d", type, proto, fd);
        return fd;
}

int nbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
        struct nsock_handle handle;
        if (addr == NULL || addrlen < sizeof(struct sockaddr_in)) {
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
        if (socket_owner_call(&cmd) != 0)
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
        if (socket_owner_call(&cmd) != 0)
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
        if (socket_owner_call(&cmd) != 0)
                return -1;
        return cmd.result;
}

ssize_t nrecvfrom(int sockfd, void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, socklen_t *addrlen) {
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
        if (socket_owner_call(&cmd) != 0)
                return -1;
        return cmd.result;
}

int nclose(int sockfd) {
        struct nsock_handle handle;
        if (fd_take(sockfd, &handle) != 0) {
                errno = EBADF;
                return -1;
        }

        struct sock_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = SOCK_CMD_CLOSE;
        cmd.handle = handle;
        return socket_owner_call(&cmd);
}

int nconnect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
        struct nsock_handle handle;
        if (addr == NULL || addrlen < sizeof(struct sockaddr_in)) {
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
        return socket_owner_call(&cmd);
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
        struct nsock_handle handle;
        if (fd_resolve(sockfd, &handle) != 0) {
                errno = EBADF;
                return -1;
        }

        struct sock_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = SOCK_CMD_ACCEPT;
        cmd.handle = handle;
        cmd.args.address.out_addr = addr;
        cmd.args.address.out_addrlen = addrlen;
        if (socket_owner_call(&cmd) != 0)
                return -1;

        int child_fd = fd_publish(cmd.result_handle);
        if (child_fd >= 0)
                return child_fd;

        /* fd exhaustion must not orphan the accepted owner-side child. */
        struct sock_cmd close_cmd;
        memset(&close_cmd, 0, sizeof(close_cmd));
        close_cmd.type = SOCK_CMD_CLOSE;
        close_cmd.handle = cmd.result_handle;
        (void)socket_owner_call(&close_cmd);
        errno = EMFILE;
        return -1;
}
