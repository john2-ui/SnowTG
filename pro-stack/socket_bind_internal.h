/**
 * @file socket_bind_internal.h
 * @brief Process-wide endpoint reservations without cross-owner TCB pointers.
 *
 * Mutation is owner-only; classifiers may copy selected handles. Records
 * preserve bind-time SO_REUSEADDR and order until the socket is destroyed.
 * Established TCP four-tuples remain in the existing transport indexes.
 */
#ifndef NETARCH_SOCKET_BIND_INTERNAL_H
#define NETARCH_SOCKET_BIND_INTERNAL_H
#include "socket_owner.h"
struct nsock;
/** Reserve an endpoint, returning 0 or a negative errno. */
int socket_bind_add(struct nsock *sk, uint32_t ip, uint16_t port);
/** Remove exactly this generation and reveal older shared UDP bindings. */
void socket_bind_remove(struct nsock *sk);
/** Atomically reject overlapping active listeners before publication. */
int socket_bind_listen(struct nsock *sk, bool enabled);
/** Test exact/wildcard reservation overlap for implicit port allocation. */
bool socket_bind_taken(uint8_t protocol, uint32_t ip, uint16_t port);
/** Fast shared-port presence test; ordinary UDP keeps its owner-local hash. */
bool socket_bind_udp_shared(uint16_t port);
/** Copy exact-address-first, newest-bind-first UDP identity under the lock. */
bool socket_bind_udp_select(uint32_t ip, uint16_t port,
                            struct nsock_handle *handle);
#endif
