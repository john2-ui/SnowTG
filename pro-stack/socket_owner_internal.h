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
struct tcp_owner_memory;
struct udp_owner_memory;

/** Resolve a generation-checked handle on its owner lcore, or set errno. */
struct nsock *socket_owner_resolve_local(struct nsock_handle handle);
/** Number of generation-protected object slots owned by the current lcore. */
uint32_t socket_owner_slot_capacity_local(void);
/** Return one current owner-local object by slot, or NULL for a vacant slot. */
struct nsock *socket_owner_slot_at_local(uint32_t id);
/** Coalesce readiness bits for a live owner-local socket. */
void socket_owner_ready_post(struct nsock *sk, uint32_t events);
/** Remove up to @p max_events coalesced readiness records from the owner queue.
 */
unsigned int socket_owner_ready_burst(struct owner_io_event *events,
                                      unsigned int max_events);
/** Return the current lcore's TCP memory domain, or NULL outside its owner. */
struct tcp_owner_memory *socket_owner_tcp_memory(void);
/** Return the current lcore's UDP memory domain, or NULL outside its owner. */
struct udp_owner_memory *socket_owner_udp_memory(void);
/** Snapshot the owner-local TCP resource budget for a co-located reactor. */
int socket_owner_tcp_memory_snapshot(struct tcp_memory_snapshot *snapshot);
/** Non-zero when this owner must pause new connection admission. */
int socket_owner_tcp_memory_below_low_water(void);
/** Non-zero when this owner has cleared every resource high-water mark. */
int socket_owner_tcp_memory_above_high_water(void);

#endif /* NETARCH_SOCKET_OWNER_INTERNAL_H */
