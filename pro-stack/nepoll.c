/**
 * @file nepoll.c
 * @brief Public level-triggered poll sets over generation-checked readiness
 * snapshots.
 */
#include "nepoll.h"
#include "socket.h"
#include "socket_public_internal.h"
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* ponytail: persistent subscription slots replace a lossy notification ring.
 * Wait scans registered slots (bounded by NSOCK_FD_MAX); add a ready index if
 * profiling large public poll sets justifies it. Owner-local reactors bypass
 * it. */
struct subscription {
        bool used;
        struct nsock_handle handle;
        uint32_t interest;
        uint64_t data;
};
struct poller {
        int id;
        unsigned refs, cursor;
        bool closed;
        struct poller *next;
        struct subscription slots[NSOCK_FD_MAX];
};
static pthread_mutex_t poll_lock = PTHREAD_MUTEX_INITIALIZER;
static struct poller *pollers;
static unsigned next_id = 1;
atomic_uint socket_public_sequence;

void socket_public_notify(void) {
        atomic_fetch_add_explicit(&socket_public_sequence, 1,
                                  memory_order_release);
        syscall(SYS_futex, &socket_public_sequence, FUTEX_WAKE_PRIVATE, INT_MAX,
                NULL, NULL, 0);
}
static struct poller *find(int id) {
        for (struct poller *p = pollers; p; p = p->next)
                if (p->id == id)
                        return p;
        return NULL;
}
static bool same(struct nsock_handle a, struct nsock_handle b) {
        return a.id == b.id && a.generation == b.generation &&
               a.owner_lcore == b.owner_lcore && a.protocol == b.protocol;
}
static uint64_t now_ns(void) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
int nepoll_create(void) {
        struct poller *p = calloc(1, sizeof(*p));
        if (!p) {
                errno = ENOMEM;
                return -1;
        }
        pthread_mutex_lock(&poll_lock);
        if (next_id > INT_MAX) {
                pthread_mutex_unlock(&poll_lock);
                free(p);
                errno = EMFILE;
                return -1;
        }
        p->id = next_id++;
        p->refs = 1;
        p->next = pollers;
        pollers = p;
        int id = p->id;
        pthread_mutex_unlock(&poll_lock);
        return id;
}
int nepoll_ctl(int epfd, int operation, int fd,
               const struct nepoll_event *event) {
        if (fd < 0 || fd >= NSOCK_FD_MAX || operation < NEPOLL_CTL_ADD ||
            operation > NEPOLL_CTL_DEL ||
            (operation != NEPOLL_CTL_DEL &&
             (!event || (event->events & ~63U)))) {
                errno = EINVAL;
                return -1;
        }
        int error = 0;
        pthread_mutex_lock(&poll_lock);
        struct poller *p = find(epfd);
        struct nsock_handle handle;
        uint32_t ready;
        if (!p || socket_public_snapshot(fd, &handle, &ready))
                error = EBADF;
        else {
                struct subscription *s = &p->slots[fd];
                if (s->used && !same(s->handle, handle))
                        s->used = false;
                if (operation == NEPOLL_CTL_ADD && s->used)
                        error = EEXIST;
                else if (operation != NEPOLL_CTL_ADD && !s->used)
                        error = ENOENT;
                else if (operation == NEPOLL_CTL_DEL)
                        s->used = false;
                else
                        *s = (struct subscription){true, handle, event->events,
                                                   event->data};
        }
        pthread_mutex_unlock(&poll_lock);
        if (error) {
                errno = error;
                return -1;
        }
        socket_public_notify();
        return 0;
}
static void release(void *arg) {
        struct poller *p = arg;
        pthread_mutex_lock(&poll_lock);
        bool destroy = --p->refs == 0;
        pthread_mutex_unlock(&poll_lock);
        if (destroy)
                free(p);
}
int nepoll_wait(int epfd, struct nepoll_event *events, int maxevents,
                int timeout_ms) {
        if (!events || maxevents <= 0 || timeout_ms < -1) {
                errno = EINVAL;
                return -1;
        }
        pthread_mutex_lock(&poll_lock);
        struct poller *p = find(epfd);
        if (p)
                p->refs++;
        pthread_mutex_unlock(&poll_lock);
        if (!p) {
                errno = EBADF;
                return -1;
        }
        uint64_t deadline = timeout_ms < 0
                                ? UINT64_MAX
                                : now_ns() + (uint64_t)timeout_ms * 1000000;
        int count = 0, error = 0;
        pthread_cleanup_push(release, p);
        for (;;) {
                unsigned seq = atomic_load_explicit(&socket_public_sequence,
                                                    memory_order_acquire);
                pthread_mutex_lock(&poll_lock);
                if (p->closed)
                        error = EBADF;
                unsigned start = p->cursor;
                for (unsigned i = 0;
                     !error && i < NSOCK_FD_MAX && count < maxevents; i++) {
                        unsigned fd = (start + i) % NSOCK_FD_MAX;
                        struct subscription *s = &p->slots[fd];
                        if (!s->used)
                                continue;
                        struct nsock_handle handle;
                        uint32_t mask;
                        if (socket_public_snapshot(fd, &handle, &mask) ||
                            !same(handle, s->handle)) {
                                s->used = false;
                                continue;
                        }
                        mask &= s->interest | NEPOLL_ERROR | NEPOLL_HUP;
                        if (mask) {
                                events[count++] =
                                    (struct nepoll_event){mask, fd, s->data};
                                p->cursor = (fd + 1) % NSOCK_FD_MAX;
                        }
                }
                pthread_mutex_unlock(&poll_lock);
                if (error || count || now_ns() >= deadline)
                        break;
                uint64_t remaining = deadline - now_ns();
                if (remaining > 10000000)
                        remaining = 10000000;
                struct timespec timeout = {.tv_nsec = remaining};
                syscall(SYS_futex, &socket_public_sequence, FUTEX_WAIT_PRIVATE,
                        seq, &timeout, NULL, 0);
                pthread_testcancel();
        }
        pthread_cleanup_pop(1);
        if (error) {
                errno = error;
                return -1;
        }
        return count;
}
int nepoll_close(int epfd) {
        pthread_mutex_lock(&poll_lock);
        struct poller **link = &pollers;
        while (*link && (*link)->id != epfd)
                link = &(*link)->next;
        struct poller *p = *link;
        if (!p) {
                pthread_mutex_unlock(&poll_lock);
                errno = EBADF;
                return -1;
        }
        *link = p->next;
        p->closed = true;
        bool destroy = --p->refs == 0;
        pthread_mutex_unlock(&poll_lock);
        socket_public_notify();
        if (destroy)
                free(p);
        return 0;
}
