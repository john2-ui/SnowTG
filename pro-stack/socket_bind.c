/**
 * @file socket_bind.c
 * @brief Shared bind reservations and deterministic UDP recipient selection.
 */
#include "rx_dispatch.h"
#include "socket.h"
#include "socket_bind_internal.h"
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>

/* Binding changes are cold-path and serialized process-wide. The hash is
 * keyed by protocol/port so wildcard conflicts are checked in the same bucket.
 */
#define BIND_BUCKETS 1024
struct binding {
        struct binding *next;
        struct nsock_handle handle;
        uint32_t ip;
        uint16_t port;
        bool reuse, listener;
};
static struct binding *bindings[BIND_BUCKETS];
static atomic_uint shared_udp[65536];
bool socket_bind_udp_shared(uint16_t port) {
        return atomic_load(&shared_udp[port]) != 0;
}
static pthread_mutex_t bind_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned bucket(uint8_t proto, uint16_t port) {
        return ((unsigned)ntohs(port) * 31 + proto) % BIND_BUCKETS;
}
static bool same(struct nsock_handle a, struct nsock_handle b) {
        return a.id == b.id && a.generation == b.generation &&
               a.owner_lcore == b.owner_lcore && a.protocol == b.protocol;
}
static bool overlap(uint32_t a, uint32_t b) { return !a || !b || a == b; }
static struct binding *select_udp(uint32_t ip, uint16_t port) {
        struct binding *wildcard = NULL;
        for (struct binding *b = bindings[bucket(IPPROTO_UDP, port)]; b;
             b = b->next) {
                if (b->port != port || b->handle.protocol != IPPROTO_UDP)
                        continue;
                if (b->ip == ip)
                        return b;
                if (!b->ip && !wildcard)
                        wildcard = b;
        }
        return wildcard;
}
bool socket_bind_udp_select(uint32_t ip, uint16_t port,
                            struct nsock_handle *handle) {
        pthread_mutex_lock(&bind_lock);
        struct binding *b = select_udp(ip, port);
        if (b)
                *handle = b->handle;
        pthread_mutex_unlock(&bind_lock);
        return b != NULL;
}
int socket_bind_add(struct nsock *sk, uint32_t ip, uint16_t port) {
        struct binding *entry = calloc(1, sizeof(*entry));
        if (!entry)
                return -ENOMEM;
        *entry = (struct binding){.handle = socket_owner_handle(sk),
                                  .ip = ip,
                                  .port = port,
                                  .reuse = sk->reuseaddr};
        unsigned index = bucket(sk->protocol, port);
        int rc = 0;
        pthread_mutex_lock(&bind_lock);
        for (struct binding *b = bindings[index]; b; b = b->next) {
                if (b->port != port || b->handle.protocol != sk->protocol ||
                    !overlap(b->ip, ip))
                        continue;
                if (!b->reuse || !entry->reuse || b->listener) {
                        rc = -EADDRINUSE;
                        break;
                }
        }
        if (!rc) {
                entry->next = bindings[index];
                bindings[index] = entry;
                if (sk->protocol == IPPROTO_UDP && sk->reuseaddr)
                        atomic_fetch_add(&shared_udp[port], 1);
        }
        pthread_mutex_unlock(&bind_lock);
        if (rc)
                free(entry);
        return rc;
}
void socket_bind_remove(struct nsock *sk) {
        unsigned index = bucket(sk->protocol, sk->local_port);
        struct nsock_handle handle = socket_owner_handle(sk);
        pthread_mutex_lock(&bind_lock);
        struct binding **link = &bindings[index];
        while (*link && !same((*link)->handle, handle))
                link = &(*link)->next;
        if (*link) {
                struct binding *old = *link;
                *link = old->next;
                if (old->handle.protocol == IPPROTO_UDP && old->reuse)
                        atomic_fetch_sub(&shared_udp[old->port], 1);
                free(old);
        }
        pthread_mutex_unlock(&bind_lock);
}
bool socket_bind_taken(uint8_t proto, uint32_t ip, uint16_t port) {
        bool taken = false;
        pthread_mutex_lock(&bind_lock);
        for (struct binding *b = bindings[bucket(proto, port)]; b; b = b->next)
                if (b->port == port && b->handle.protocol == proto &&
                    overlap(b->ip, ip)) {
                        taken = true;
                        break;
                }
        pthread_mutex_unlock(&bind_lock);
        return taken;
}
int socket_bind_listen(struct nsock *sk, bool enabled) {
        struct nsock_handle handle = socket_owner_handle(sk);
        struct binding *self = NULL;
        int rc = 0;
        pthread_mutex_lock(&bind_lock);
        for (struct binding *b = bindings[bucket(IPPROTO_TCP, sk->local_port)];
             b; b = b->next) {
                if (same(b->handle, handle))
                        self = b;
                else if (enabled && b->handle.protocol == IPPROTO_TCP &&
                         b->port == sk->local_port &&
                         overlap(b->ip, sk->local_ip) && b->listener)
                        rc = -EADDRINUSE;
        }
        if (!self)
                rc = -EINVAL;
        if (!rc)
                self->listener = enabled;
        pthread_mutex_unlock(&bind_lock);
        return rc;
}
