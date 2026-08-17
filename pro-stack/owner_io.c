#include "owner_io.h"

#include "socket.h"
#include "socket_owner_internal.h"

#include <errno.h>
#include <netinet/in.h>
#include <rte_lcore.h>

static struct nsock *owner_io_resolve(struct nsock_handle handle) {
        return socket_owner_resolve_local(handle);
}

/* The owner-local API currently exposes IPv4 endpoints only. */
static int owner_io_validate_ipv4_addr(const struct sockaddr *addr,
                                       socklen_t addrlen) {
        if (addr == NULL || addrlen < sizeof(struct sockaddr_in) ||
            addr->sa_family != AF_INET) {
                errno = EINVAL;
                return -1;
        }
        return 0;
}

static int owner_io_socket_create_mode(uint8_t protocol,
                                       enum nsock_io_mode io_mode,
                                       struct nsock_handle *out) {
        if (out == NULL) {
                errno = EINVAL;
                return -1;
        }

        struct nsock *sk = nsock_alloc_mode(protocol, io_mode);
        if (sk == NULL) {
                errno = EPROTONOSUPPORT;
                return -1;
        }

        int rc = socket_owner_adopt(sk);
        if (rc != 0) {
                nsock_free(sk);
                errno = -rc;
                return -1;
        }

        /* Owner-local flows never receive a public fd-table entry. */
        sk->app_visible = false;
        sk->app_closed = false;
        *out = socket_owner_handle(sk);
        return 0;
}

int owner_io_socket_create(uint8_t protocol, struct nsock_handle *out) {
        return owner_io_socket_create_mode(protocol, NSOCK_IO_RINGS, out);
}

int owner_io_socket_create_local(uint8_t protocol, struct nsock_handle *out) {
        return owner_io_socket_create_mode(protocol, NSOCK_IO_OWNER_LOCAL, out);
}

int owner_io_set_release_observer(struct nsock_handle handle,
                                  owner_io_release_fn fn, void *ctx) {
        struct nsock *sk = owner_io_resolve(handle);

        if (sk == NULL)
                return -1;
        nsock_set_release_observer(sk, fn, ctx);
        return 0;
}

int owner_io_bind(struct nsock_handle handle, const struct sockaddr *addr,
                  socklen_t addrlen) {
        struct nsock *sk = owner_io_resolve(handle);
        if (sk == NULL)
                return -1;
        if (owner_io_validate_ipv4_addr(addr, addrlen) != 0)
                return -1;

        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        int rc = nsock_bind_local(sk, sin->sin_addr.s_addr, sin->sin_port);
        if (rc != 0) {
                errno = -rc;
                return -1;
        }
        return 0;
}

int owner_io_bind_ephemeral(struct nsock_handle handle, uint32_t local_ip) {
        struct nsock *sk = owner_io_resolve(handle);
        int rc;

        if (sk == NULL)
                return -1;
        if (sk->protocol != IPPROTO_UDP) {
                errno = EOPNOTSUPP;
                return -1;
        }
        if (local_ip == INADDR_ANY) {
                errno = EADDRNOTAVAIL;
                return -1;
        }

        rc = nsock_udp_bind_ephemeral(sk, local_ip);
        if (rc != 0) {
                errno = rc < 0 ? -rc : EIO;
                return -1;
        }
        return 0;
}

int owner_io_connect(struct nsock_handle handle, const struct sockaddr *addr,
                     socklen_t addrlen) {
        struct nsock *sk = owner_io_resolve(handle);
        if (sk == NULL)
                return -1;
        if (owner_io_validate_ipv4_addr(addr, addrlen) != 0)
                return -1;
        if (sk->ops->connect == NULL) {
                errno = EOPNOTSUPP;
                return -1;
        }
        return sk->ops->connect(sk, addr, addrlen);
}

ssize_t owner_io_send(struct nsock_handle handle, const void *buf, size_t len) {
        struct nsock *sk = owner_io_resolve(handle);
        if (sk == NULL)
                return -1;
        if ((buf == NULL && len != 0) || sk->ops->send == NULL) {
                errno = buf == NULL && len != 0 ? EINVAL : EOPNOTSUPP;
                return -1;
        }
        return sk->ops->send(sk, buf, len, MSG_DONTWAIT);
}

ssize_t owner_io_recv(struct nsock_handle handle, void *buf, size_t len) {
        struct nsock *sk = owner_io_resolve(handle);
        if (sk == NULL)
                return -1;
        if ((buf == NULL && len != 0) || sk->ops->recv == NULL) {
                errno = buf == NULL && len != 0 ? EINVAL : EOPNOTSUPP;
                return -1;
        }
        return sk->ops->recv(sk, buf, len, MSG_DONTWAIT);
}

ssize_t owner_io_sendto(struct nsock_handle handle, const void *buf, size_t len,
                        const struct sockaddr *addr, socklen_t addrlen) {
        struct nsock *sk = owner_io_resolve(handle);
        if (sk == NULL)
                return -1;
        if ((buf == NULL && len != 0) || sk->ops->sendto == NULL) {
                errno = buf == NULL && len != 0 ? EINVAL : EOPNOTSUPP;
                return -1;
        }
        if (owner_io_validate_ipv4_addr(addr, addrlen) != 0)
                return -1;
        return sk->ops->sendto(sk, buf, len, MSG_DONTWAIT, addr, addrlen);
}

ssize_t owner_io_recvfrom(struct nsock_handle handle, void *buf, size_t len,
                          struct sockaddr *addr, socklen_t *addrlen) {
        struct nsock *sk = owner_io_resolve(handle);
        if (sk == NULL)
                return -1;
        if ((buf == NULL && len != 0) || sk->ops->recvfrom == NULL) {
                errno = buf == NULL && len != 0 ? EINVAL : EOPNOTSUPP;
                return -1;
        }
        if (addr != NULL &&
            (addrlen == NULL || *addrlen < sizeof(struct sockaddr_in))) {
                errno = EINVAL;
                return -1;
        }

        ssize_t result =
            sk->ops->recvfrom(sk, buf, len, MSG_DONTWAIT, addr, addrlen);
        if (result >= 0 && addr != NULL)
                *addrlen = sizeof(struct sockaddr_in);
        return result;
}

int owner_io_close(struct nsock_handle handle) {
        struct nsock *sk = owner_io_resolve(handle);
        if (sk == NULL)
                return -1;
        if (sk->ops->close == NULL) {
                errno = EOPNOTSUPP;
                return -1;
        }

        sk->app_closed = true;
        socket_owner_abort_waiters(sk, ECANCELED);
        return sk->ops->close(sk);
}

unsigned int owner_io_ready_burst(struct owner_io_event *events,
                                  unsigned int max_events) {
        return socket_owner_ready_burst(events, max_events);
}

/** @copydoc owner_io_memory_snapshot */
int owner_io_memory_snapshot(struct owner_io_memory_snapshot *snapshot) {
        if (snapshot == NULL ||
            socket_owner_tcp_memory_snapshot(&snapshot->tcp) != 0) {
                errno = EPERM;
                return -1;
        }
        snapshot->below_low_water = socket_owner_tcp_memory_below_low_water();
        snapshot->above_high_water = socket_owner_tcp_memory_above_high_water();
        return 0;
}
