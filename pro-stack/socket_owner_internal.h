#ifndef NETARCH_SOCKET_OWNER_INTERNAL_H
#define NETARCH_SOCKET_OWNER_INTERNAL_H

/*
 * Private boundary between owner lifecycle code and owner_io.c.  This header
 * is intentionally not included by traffic-gen or other upper layers.
 */

#include "socket_owner.h"

#include <stdint.h>

struct nsock;
struct owner_io_event;

/** Resolve a generation-checked handle on its owner lcore, or set errno. */
struct nsock *socket_owner_resolve_local(struct nsock_handle handle);
/** Coalesce readiness bits for a live owner-local socket. */
void socket_owner_ready_post(struct nsock *sk, uint32_t events);
/** Remove up to @p max_events coalesced readiness records from the owner queue. */
unsigned int socket_owner_ready_burst(struct owner_io_event *events,
                                      unsigned int max_events);

#endif /* NETARCH_SOCKET_OWNER_INTERNAL_H */
