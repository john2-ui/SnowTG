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

#include <netinet/in.h>
#include <pthread.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_ring.h>
#include <rte_timer.h>
#include <stdatomic.h>
#include <string.h>

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

/* TODO: fd_alloc/fd_release and the g_sock_list mutations in nsock_alloc /
 * nsock_free are not locked. The app lcore (nsocket/nclose) and the worker
 * lcore (tcp_stream_create) can race on both the bitmap and the list. Add a
 * mutex (or a lock-free fd allocator) before relying on concurrent sockets. */
int fd_alloc(void) {
        for (int i = 0; i < NSOCK_FD_MAX; i++) {
                uint8_t mask = (uint8_t)(1u << (i % 8));
                uint8_t *slot = &fd_bitmap[i / 8];
                if ((*slot & mask) == 0) {
                        *slot |= mask;
                        return i;
                }
        }
        return -1;
}

void fd_release(int fd) {
        if (fd < 0 || fd >= NSOCK_FD_MAX)
                return;
        fd_bitmap[fd / 8] &= (uint8_t)~(1u << (fd % 8));
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
        }

        rte_memcpy(sk->local_mac, g_net.local_mac, RTE_ETHER_ADDR_LEN);

        LL_ADD(sk, g_sock_list);
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
        LL_REMOVE(sk, g_sock_list);
        pthread_cond_destroy(&sk->cond);
        pthread_mutex_destroy(&sk->mutex);
        rte_ring_free(sk->recv_buf);
        rte_ring_free(sk->send_buf);
        fd_release(sk->fd);
        rte_free(sk);
}

struct nsock *nsock_from_fd(int fd) {
        if (fd < 0)
                return NULL;
        for (struct nsock *sk = g_sock_list; sk != NULL; sk = sk->next) {
                if (sk->fd == fd)
                        return sk;
        }
        return NULL;
}

struct nsock *nsock_from_ip_port(uint32_t ip, uint16_t port, uint8_t protocol) {
        for (struct nsock *sk = g_sock_list; sk != NULL; sk = sk->next) {
                if (sk->protocol == protocol && sk->local_ip == ip &&
                    sk->local_port == port)
                        return sk;
        }
        return NULL;
}

struct nsock *nsock_from_4tuple(uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port,
                                uint8_t protocol) {
        for (struct nsock *sk = g_sock_list; sk != NULL; sk = sk->next) {
                if (sk->protocol != protocol)
                        continue;
                if (sk->u.tcp.remote_ip == remote_ip &&
                    sk->local_ip == local_ip &&
                    sk->u.tcp.remote_port == remote_port &&
                    sk->local_port == local_port)
                        return sk;
        }
        return NULL;
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
        sk->local_port = laddr->sin_port;
        sk->local_ip = laddr->sin_addr.s_addr;

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
