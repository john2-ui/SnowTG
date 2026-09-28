/**
 * @file socket_public_internal.h
 * @brief Owner-produced readiness snapshots consumed by public pollers.
 *
 * Only refresh reads a TCB, on its owner. Snapshot readers copy descriptor
 * identity and readiness under the fd-table mutex. Notification is a sequence
 * change, never the source of truth, so coalescing cannot lose readiness.
 */
#ifndef NETARCH_SOCKET_PUBLIC_INTERNAL_H
#define NETARCH_SOCKET_PUBLIC_INTERNAL_H
#include "socket_owner.h"
struct nsock;
/** Recompute readiness and configuration after owner-side progress. */
void socket_public_refresh(struct nsock *sk);
/** Wake snapshot consumers after readiness, registration or close changes. */
void socket_public_notify(void);
/** Copy one live descriptor snapshot; return -1 if it has been detached. */
int socket_public_snapshot(int fd, struct nsock_handle *handle,
                           uint32_t *events);
extern atomic_uint socket_public_sequence;
#endif
