/**
 * @file socket.c
 * @brief Unified socket registry, fd table, and BSD-style API dispatchers.
 *
 * This file replaces the old net_addr/socket_api pair. It owns the single
 * socket list @ref g_sock_list, a real fd bitmap allocator, and the common
 * nsock lifecycle (rings, lock, cond, list linkage). Every public API call
 * resolves the fd to a @ref nsock and forwards to @c sk->ops->... , so the
 * transport implementations stay out of the dispatch surface.
 */
#include "socket.h"

#include "config.h"
#include "list.h"
#include "log.h"
#include "net_context.h"
#include "tcp.h"

#include <asm-generic/errno-base.h>
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

static struct nsock *fd_table[NSOCK_FD_MAX];

static struct rte_hash *udp_bind_hash;
static struct rte_hash *tcp_bind_hash;
static struct rte_hash *tcp_listener_hash;
static struct rte_hash *tcp_conn_hash;

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static int registry_ready;

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

static struct rte_hash *registry_hash_create(const char *name,
                                             uint32_t key_len) {
        const struct rte_hash_parameters p = {
            .name = name,
            .entries = NSOCK_REGISTRY_ENTRIES,
            .key_len = key_len,
            .hash_func = rte_jhash,
            .hash_func_init_val = 0,
            .socket_id = rte_socket_id(),
        };
        return rte_hash_create(&p);
}

int socket_registry_init(void) {
        pthread_mutex_lock(&registry_lock);

        if (registry_ready) {
                pthread_mutex_unlock(&registry_lock);
                return 0;
        }

        udp_bind_hash =
            registry_hash_create("nsock_udp_bind", sizeof(struct local_key));
        tcp_bind_hash =
            registry_hash_create("nsock_tcp_bind", sizeof(struct local_key));
        tcp_listener_hash = registry_hash_create("nsock_tcp_listener",
                                                 sizeof(struct local_key));
        tcp_conn_hash =
            registry_hash_create("nsock_tcp_conn", sizeof(struct tcp_conn_key));

        if (udp_bind_hash == NULL || tcp_bind_hash == NULL ||
            tcp_listener_hash == NULL || tcp_conn_hash == NULL) {
                if (udp_bind_hash != NULL)
                        rte_hash_free(udp_bind_hash);
                if (tcp_bind_hash != NULL)
                        rte_hash_free(tcp_bind_hash);
                if (tcp_listener_hash != NULL)
                        rte_hash_free(tcp_listener_hash);
                if (tcp_conn_hash != NULL)
                        rte_hash_free(tcp_conn_hash);

                udp_bind_hash = NULL;
                tcp_bind_hash = NULL;
                tcp_listener_hash = NULL;
                tcp_conn_hash = NULL;

                pthread_mutex_unlock(&registry_lock);
                return -1;
        }

        registry_ready = 1;
        pthread_mutex_unlock(&registry_lock);
        return 0;
}

void socket_registry_fini(void) {
        pthread_mutex_lock(&registry_lock);

        if (tcp_conn_hash != NULL)
                rte_hash_free(tcp_conn_hash);
        if (tcp_listener_hash != NULL)
                rte_hash_free(tcp_listener_hash);
        if (tcp_bind_hash != NULL)
                rte_hash_free(tcp_bind_hash);
        if (udp_bind_hash != NULL)
                rte_hash_free(udp_bind_hash);

        tcp_conn_hash = NULL;
        tcp_listener_hash = NULL;
        tcp_bind_hash = NULL;
        udp_bind_hash = NULL;

        registry_ready = 0;
        pthread_mutex_unlock(&registry_lock);
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
        struct rte_hash *hash;
        struct local_key new_key;
        uint8_t flag;
        int rc;

        if (sk == NULL || port == 0)
                return -EINVAL;

        if (sk->protocol == IPPROTO_UDP) {
                hash = udp_bind_hash;
                flag = NSOCK_REG_UDP_BIND;
        } else if (sk->protocol == IPPROTO_TCP) {
                hash = tcp_bind_hash;
                flag = NSOCK_REG_TCP_BIND;
        } else {
                return -EINVAL;
        }

        new_key = local_key_make(ip, port);

        pthread_mutex_lock(&registry_lock);

        if (sk->registry_flags & flag) {
                pthread_mutex_unlock(&registry_lock);
                return -EINVAL;
        }

        rc = hash_add_unique(hash, &new_key, sk);
        if (rc == 0) {
                sk->local_ip = ip;
                sk->local_port = port;
                sk->registry_flags |= flag;
        }

        pthread_mutex_unlock(&registry_lock);
        return rc;
}

int nsock_tcp_local_taken(uint32_t ip, uint16_t port) {
        struct local_key key = local_key_make(ip, port);
        void *data = NULL;
        int found;

        pthread_mutex_lock(&registry_lock);
        found = rte_hash_lookup_data(tcp_bind_hash, &key, &data) >= 0;
        pthread_mutex_unlock(&registry_lock);

        return found;
}

int nsock_tcp_listener_register(struct nsock *sk) {
        struct local_key key;
        int rc;

        if (sk == NULL || sk->protocol != IPPROTO_TCP ||
            !(sk->registry_flags & NSOCK_REG_TCP_BIND))
                return -EINVAL;

        key = local_key_make(sk->local_ip, sk->local_port);

        pthread_mutex_lock(&registry_lock);
        rc = hash_add_unique(tcp_listener_hash, &key, sk);
        if (rc == 0) {
                sk->registry_flags |= NSOCK_REG_TCP_LISTENER;
        }
        pthread_mutex_unlock(&registry_lock);
        return rc;
}

void nsock_tcp_listener_unregister(struct nsock *sk) {
        struct local_key key;
        if (sk == NULL || sk->protocol != IPPROTO_TCP ||
            !(sk->registry_flags & NSOCK_REG_TCP_LISTENER))
                return;

        key = local_key_make(sk->local_ip, sk->local_port);
        pthread_mutex_lock(&registry_lock);
        hash_del(tcp_listener_hash, &key);
        sk->registry_flags &= (uint8_t)~NSOCK_REG_TCP_LISTENER;
        pthread_mutex_unlock(&registry_lock);
}

int nsock_tcp_conn_register(struct nsock *sk) {
        struct tcp_conn_key key;
        int rc;

        if (sk == NULL || sk->protocol != IPPROTO_TCP)
                return -EINVAL;

        key = tcp_conn_key_make(sk);

        pthread_mutex_lock(&registry_lock);
        rc = hash_add_unique(tcp_conn_hash, &key, sk);
        if (rc == 0)
                sk->registry_flags |= NSOCK_REG_TCP_CONN;
        pthread_mutex_unlock(&registry_lock);
        return rc;
}

void nsock_tcp_conn_unregister(struct nsock *sk) {
        struct tcp_conn_key key;
        if (sk == NULL || !(sk->registry_flags & NSOCK_REG_TCP_CONN))
                return;

        key = tcp_conn_key_make(sk);
        pthread_mutex_lock(&registry_lock);
        hash_del(tcp_conn_hash, &key);
        sk->registry_flags &= (uint8_t)~NSOCK_REG_TCP_CONN;
        pthread_mutex_unlock(&registry_lock);
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

struct nsock *g_sock_list = NULL;

/** fd bitmap: bit i is set while fd i is in use. */
static uint8_t fd_bitmap[NSOCK_FD_MAX / 8];

/*
 * registry_lock serializes bitmap, fd-table, hash-index, and g_sock_list
 * mutations. It does not pin a returned struct nsock pointer; callers that
 * need lock-free lookup concurrent with close still require deferred free or
 * reference counting.
 */
int fd_alloc(void) {
        pthread_mutex_lock(&registry_lock);
        for (int i = 0; i < NSOCK_FD_MAX; i++) {
                uint8_t mask = (uint8_t)(1u << (i % 8));
                uint8_t *slot = &fd_bitmap[i / 8];
                if ((*slot & mask) == 0) {
                        *slot |= mask;
                        pthread_mutex_unlock(&registry_lock);
                        return i;
                }
        }
        pthread_mutex_unlock(&registry_lock);
        return -1;
}

void fd_release(int fd) {
        if (fd < 0 || fd >= NSOCK_FD_MAX)
                return;
        pthread_mutex_lock(&registry_lock);
        fd_bitmap[fd / 8] &= (uint8_t)~(1u << (fd % 8));
        pthread_mutex_unlock(&registry_lock);
}

struct nsock *nsock_alloc(int fd, uint8_t protocol) {
        const struct sock_ops *ops = sock_ops_lookup(protocol);
        if (ops == NULL) {
                LOG_ERROR("nsock_alloc: unknown protocol %u", protocol);
                if (fd >= 0)
                        fd_release(fd);
                return NULL;
        }

        struct nsock *sk = rte_malloc("nsock", sizeof(struct nsock), 0);
        if (sk == NULL) {
                LOG_ERROR("rte_malloc(nsock) failed");
                if (fd >= 0)
                        fd_release(fd);
                return NULL;
        }
        memset(sk, 0, sizeof(*sk));

        /* fd < 0: incomplete TCP child; real fd assigned in tcp_accept. */
        sk->fd = fd;
        sk->protocol = protocol;
        sk->ops = ops;

        /* Unique ring names so a second socket does not collide. */
        static atomic_uint ring_id = 0;
        unsigned int id = atomic_fetch_add(&ring_id, 1);
        char recv_name[32], send_name[32];
        snprintf(recv_name, sizeof(recv_name), "sock_recv_%u", id);
        snprintf(send_name, sizeof(send_name), "sock_send_%u", id);

        sk->recv_buf = rte_ring_create(recv_name, RING_SIZE, rte_socket_id(),
                                       RING_F_SP_ENQ | RING_F_SC_DEQ);
        sk->send_buf = rte_ring_create(send_name, RING_SIZE, rte_socket_id(),
                                       RING_F_SC_DEQ);
        if (sk->recv_buf == NULL || sk->send_buf == NULL) {
                LOG_ERROR("rte_ring_create(nsock) failed");
                if (sk->recv_buf)
                        rte_ring_free(sk->recv_buf);
                if (sk->send_buf)
                        rte_ring_free(sk->send_buf);
                rte_free(sk);
                if (fd >= 0)
                        fd_release(fd);
                return NULL;
        }

        if (pthread_mutex_init(&sk->mutex, NULL) != 0 ||
            pthread_cond_init(&sk->cond, NULL) != 0) {
                LOG_ERROR("pthread init failed");
                rte_ring_free(sk->recv_buf);
                rte_ring_free(sk->send_buf);
                rte_free(sk);
                if (fd >= 0)
                        fd_release(fd);
                return NULL;
        }

        /* TCP starts CLOSED; timer is armed later by connect / RTO paths. */
        if (protocol == IPPROTO_TCP) {
                if (tcp_sndbuf_init(&sk->u.tcp.sndbuf, 0) != 0) {
                        LOG_ERROR("nsock_alloc: tcp_sndbuf_init failed");
                        rte_ring_free(sk->recv_buf);
                        rte_ring_free(sk->send_buf);
                        pthread_mutex_destroy(&sk->mutex);
                        pthread_cond_destroy(&sk->cond);
                        rte_free(sk);
                        if (fd >= 0)
                                fd_release(fd);
                        return NULL;
                }
                sk->u.tcp.snd_una = 0;
                sk->u.tcp.status = TCP_STATUS_CLOSED;
                rte_timer_init(&sk->u.tcp.timer);
                sk->u.tcp.retries = 0;
                sk->u.tcp.rcvbuf_size = TCP_RCVBUF_SIZE;
                sk->u.tcp.rcvbuf_used = 0;
                atomic_init(&sk->u.tcp.rx_consumed, 0);
                atomic_init(&sk->u.tcp.rx_event_pending, false);
                sk->u.tcp.rx_current = NULL;
                sk->u.tcp.snd_wnd = 0;
                sk->u.tcp.snd_wl1 = 0;
                sk->u.tcp.snd_wl2 = 0;

                rb_root_init(&sk->u.tcp.ofo_tree);
                sk->u.tcp.ofo = NULL;
                sk->u.tcp.ofo_tail = NULL;
                sk->u.tcp.ofo_count = 0;
                sk->u.tcp.ofo_bytes = 0;
        }

        rte_memcpy(sk->local_mac, g_net.local_mac, RTE_ETHER_ADDR_LEN);

        if (fd >= 0 && nsock_attach_fd(sk, fd) != 0) {
                LOG_ERROR("nsock_alloc: fd registry collision fd=%d", fd);
                if (protocol == IPPROTO_TCP)
                        tcp_sndbuf_free(&sk->u.tcp.sndbuf);
                pthread_cond_destroy(&sk->cond);
                pthread_mutex_destroy(&sk->mutex);
                rte_ring_free(sk->recv_buf);
                rte_ring_free(sk->send_buf);
                rte_free(sk);
                fd_release(fd);
                return NULL;
        }

        pthread_mutex_lock(&registry_lock);
        LL_ADD(sk, g_sock_list);
        pthread_mutex_unlock(&registry_lock);
        return sk;
}

void nsock_free(struct nsock *sk) {
        if (sk == NULL)
                return;
        /* Drop any pending SYN RTO / future TIME_WAIT timer before free. */
        if (sk->protocol == IPPROTO_TCP) {
                tcp_sndbuf_free(&sk->u.tcp.sndbuf);
                rte_timer_stop(&sk->u.tcp.timer);
        }
        pthread_mutex_lock(&registry_lock);

        if (sk->registry_flags & NSOCK_REG_TCP_CONN) {
                struct tcp_conn_key key = tcp_conn_key_make(sk);
                hash_del(tcp_conn_hash, &key);
        }

        if (sk->registry_flags & NSOCK_REG_TCP_LISTENER) {
                struct local_key key =
                    local_key_make(sk->local_ip, sk->local_port);
                hash_del(tcp_listener_hash, &key);
        }

        if (sk->registry_flags & NSOCK_REG_TCP_BIND) {
                struct local_key key =
                    local_key_make(sk->local_ip, sk->local_port);
                hash_del(tcp_bind_hash, &key);
        }

        if (sk->registry_flags & NSOCK_REG_UDP_BIND) {
                struct local_key key =
                    local_key_make(sk->local_ip, sk->local_port);
                hash_del(udp_bind_hash, &key);
        }

        if ((sk->registry_flags & NSOCK_REG_FD) && sk->fd >= 0 &&
            sk->fd < NSOCK_FD_MAX && fd_table[sk->fd] == sk)
                fd_table[sk->fd] = NULL;

        LL_REMOVE(sk, g_sock_list);
        sk->registry_flags = 0;

        pthread_mutex_unlock(&registry_lock);
        pthread_cond_destroy(&sk->cond);
        pthread_mutex_destroy(&sk->mutex);
        rte_ring_free(sk->recv_buf);
        rte_ring_free(sk->send_buf);
        fd_release(sk->fd);
        rte_free(sk);
}

int nsock_attach_fd(struct nsock *sk, int fd) {
        if (sk == NULL || fd < 0 || fd >= NSOCK_FD_MAX)
                return -EINVAL;

        pthread_mutex_lock(&registry_lock);
        if (fd_table[fd] != NULL) {
                pthread_mutex_unlock(&registry_lock);
                return -EEXIST;
        }
        sk->fd = fd;
        fd_table[fd] = sk;
        sk->registry_flags |= NSOCK_REG_FD;

        pthread_mutex_unlock(&registry_lock);
        return 0;
}

struct nsock *nsock_from_fd(int fd) {
        struct nsock *sk = NULL;
        if (fd < 0 || fd >= NSOCK_FD_MAX)
                return NULL;

        pthread_mutex_lock(&registry_lock);
        sk = fd_table[fd];
        pthread_mutex_unlock(&registry_lock);
        return sk;
}

struct nsock *nsock_from_ip_port(uint32_t ip, uint16_t port, uint8_t protocol) {
        struct rte_hash *hash;
        struct local_key key;
        struct nsock *sk = NULL;
        void *data = NULL;

        if (protocol == IPPROTO_UDP) {
                hash = udp_bind_hash;
        } else if (protocol == IPPROTO_TCP) {
                hash = tcp_listener_hash;
        } else {
                return NULL;
        }

        key = local_key_make(ip, port);

        pthread_mutex_lock(&registry_lock);
        if (rte_hash_lookup_data(hash, &key, &data) >= 0) {
                sk = (struct nsock *)data;
        }

        if (sk == NULL && ip != INADDR_ANY) {
                key = local_key_make(INADDR_ANY, port);
                if (rte_hash_lookup_data(hash, &key, &data) >= 0) {
                        sk = (struct nsock *)data;
                }
        }
        pthread_mutex_unlock(&registry_lock);
        return sk;
}

struct nsock *nsock_from_4tuple(uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port,
                                uint8_t protocol) {
        struct tcp_conn_key key;
        struct nsock *sk = NULL;
        void *data = NULL;

        if (protocol != IPPROTO_TCP)
                return NULL;

        memset(&key, 0, sizeof(key));
        key.remote_ip = remote_ip;
        key.local_ip = local_ip;
        key.remote_port = remote_port;
        key.local_port = local_port;

        pthread_mutex_lock(&registry_lock);
        if (rte_hash_lookup_data(tcp_conn_hash, &key, &data) >= 0)
                sk = (struct nsock *)data;
        pthread_mutex_unlock(&registry_lock);
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

        int fd = fd_alloc();
        if (fd < 0) {
                LOG_ERROR("nsocket: fd table full");
                return -1;
        }

        if (nsock_alloc(fd, proto) == NULL)
                return -1;

        LOG_INFO("nsocket type=%d proto=%u fd=%d", type, proto, fd);
        return fd;
}

int nbind(int sockfd, const struct sockaddr *addr,
          __attribute__((unused)) socklen_t addrlen) {
        struct nsock *sk = nsock_from_fd(sockfd);
        if (sk == NULL) {
                LOG_ERROR("nbind: bad fd=%d", sockfd);
                return -1;
        }

        const struct sockaddr_in *laddr = (const struct sockaddr_in *)addr;

        if (nsock_bind_local(sk, laddr->sin_addr.s_addr, laddr->sin_port) !=
            0) {
                LOG_ERROR("nbind: address already in use");
                return -1;
        }

        LOG_INFO("nbind fd=%d " IP_FMT ":%u proto=%u", sockfd,
                 IP_ARG(sk->local_ip), rte_be_to_cpu_16(sk->local_port),
                 sk->protocol);
        return 0;
}

ssize_t nsend(int sockfd, const void *buf, size_t len, int flags) {
        struct nsock *sk = nsock_from_fd(sockfd);
        if (sk == NULL || sk->ops->send == NULL) {
                LOG_ERROR("nsend: unsupported on fd=%d", sockfd);
                return -1;
        }
        return sk->ops->send(sk, buf, len, flags);
}

ssize_t nrecv(int sockfd, void *buf, size_t len, int flags) {
        struct nsock *sk = nsock_from_fd(sockfd);
        if (sk == NULL || sk->ops->recv == NULL) {
                LOG_ERROR("nrecv: unsupported on fd=%d", sockfd);
                return -1;
        }
        return sk->ops->recv(sk, buf, len, flags);
}

ssize_t nsendto(int sockfd, const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen) {
        struct nsock *sk = nsock_from_fd(sockfd);
        if (sk == NULL || sk->ops->sendto == NULL) {
                LOG_ERROR("nsendto: unsupported on fd=%d", sockfd);
                return -1;
        }
        return sk->ops->sendto(sk, buf, len, flags, dest_addr, addrlen);
}

ssize_t nrecvfrom(int sockfd, void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, socklen_t *addrlen) {
        struct nsock *sk = nsock_from_fd(sockfd);
        if (sk == NULL || sk->ops->recvfrom == NULL) {
                LOG_ERROR("nrecvfrom: unsupported on fd=%d", sockfd);
                return -1;
        }
        return sk->ops->recvfrom(sk, buf, len, flags, src_addr, addrlen);
}

int nclose(int sockfd) {
        struct nsock *sk = nsock_from_fd(sockfd);
        if (sk == NULL || sk->ops->close == NULL) {
                LOG_ERROR("nclose: bad fd=%d", sockfd);
                return -1;
        }
        return sk->ops->close(sk);
}

int nconnect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
        struct nsock *sk = nsock_from_fd(sockfd);
        if (sk == NULL || sk->ops->connect == NULL) {
                LOG_ERROR("nconnect: unsupported on fd=%d", sockfd);
                return -1;
        }
        return sk->ops->connect(sk, addr, addrlen);
}

int nlisten(int sockfd, int backlog) {
        struct nsock *sk = nsock_from_fd(sockfd);
        if (sk == NULL || sk->ops->listen == NULL) {
                LOG_ERROR("nlisten: unsupported on fd=%d", sockfd);
                return -1;
        }
        return sk->ops->listen(sk, backlog);
}

int naccept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
        struct nsock *sk = nsock_from_fd(sockfd);
        if (sk == NULL || sk->ops->accept == NULL) {
                LOG_ERROR("naccept: unsupported on fd=%d", sockfd);
                return -1;
        }
        return sk->ops->accept(sk, addr, addrlen);
}
