/**
 * @file nepoll.h
 * @brief Level-triggered, cross-owner readiness for public socket descriptors.
 *
 * Poller IDs belong to a separate namespace from n* socket descriptors. Calls
 * may run on application threads; no caller may dereference transport state.
 * Subscriptions retain the complete socket generation, so fd reuse never
 * transfers an old registration to a newly created connection. ET/ONESHOT,
 * nested pollers and host-kernel file descriptors are not supported.
 */
#ifndef NETARCH_NEPOLL_H
#define NETARCH_NEPOLL_H
#include <stdint.h>

/* Level-triggered readiness. ERROR/HUP are returned regardless of interest. */
enum nepoll_events {
        NEPOLL_READ = 1u << 0,
        NEPOLL_WRITE = 1u << 1,
        NEPOLL_CONNECTED = 1u << 2,
        NEPOLL_ERROR = 1u << 3,
        NEPOLL_HUP = 1u << 4,
        NEPOLL_ACCEPT = 1u << 5
};
enum nepoll_operation { NEPOLL_CTL_ADD = 1, NEPOLL_CTL_MOD, NEPOLL_CTL_DEL };
struct nepoll_event {
        uint32_t events;
        int fd;
        uint64_t data;
};
/** @brief Create an empty poll set.
 * @return Positive poller ID, or -1 with ENOMEM/EMFILE on failure.
 */
int nepoll_create(void);
/** @brief Add, modify or remove one generation-checked socket subscription.
 * @param event Interest mask and opaque data; ignored for DEL. Its fd field is
 * ignored on input. Unsupported event bits return EINVAL.
 * @return 0 on success; -1 with EBADF, EEXIST, ENOENT or EINVAL on failure.
 */
int nepoll_ctl(int epfd, int operation, int sockfd,
               const struct nepoll_event *event);
/** @brief Return currently ready subscriptions, fairly rotating the scan.
 * @param timeout_ms -1 waits indefinitely, 0 probes, positive values bound the
 * wait using CLOCK_MONOTONIC. State can change before the application acts;
 * nonblocking I/O must still handle EAGAIN.
 * @return Event count, 0 on timeout, or -1 with errno. Concurrent poller close
 * wakes waiters with EBADF. Thread cancellation releases the waiter reference.
 */
int nepoll_wait(int epfd, struct nepoll_event *events, int maxevents,
                int timeout_ms);
/** @brief Detach all subscriptions and wake waiters before releasing storage.
 * @return 0 on success, -1/EBADF for an invalid poller ID.
 */
int nepoll_close(int epfd);
#endif
