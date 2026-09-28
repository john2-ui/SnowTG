/** Exercise the real example through scripted public API calls, without a NIC.
 * Unexpected calls fail immediately; a final poll failure ends the server and
 * verifies cleanup. No production-only test entry point is needed.
 */
#include "../apps/tcp-echo/tcp_echo.h"
#include "../pro-stack/nepoll.h"
#include "../pro-stack/net_context.h"
#include "../pro-stack/socket.h"
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LISTENER 1
#define POLLER 99
#define LIMIT 32
#define BUFFER 1280
#define MAX_FD 64

enum operation { OP_WAIT, OP_ACCEPT, OP_RECV, OP_SEND };
struct event_spec {
        int fd;
        uint32_t mask;
        bool stale;
};
struct step {
        enum operation operation;
        int fd, result, error;
        const char *bytes;
        size_t length;
        struct event_spec events[LIMIT + 1];
};
static struct step script[512];
static unsigned length, cursor;
static bool opened[MAX_FD], registered[MAX_FD], poller_open;
static struct nepoll_event subscriptions[MAX_FD], retired[MAX_FD];
static unsigned reads[MAX_FD], writes[MAX_FD], accepts;
static unsigned ctl_calls, fail_ctl;
static enum { NONE, SOCKET_FAIL, POLLER_FAIL, BIND_FAIL, LISTEN_FAIL } fault;
struct net_context g_net;

static struct step *add(enum operation op, int fd, int result, int error) {
        assert(length < sizeof(script) / sizeof(script[0]));
        struct step *s = &script[length++];
        *s = (struct step){
            .operation = op, .fd = fd, .result = result, .error = error};
        return s;
}

static struct step *next(enum operation op, int fd) {
        assert(cursor < length);
        struct step *s = &script[cursor++];
        if (s->operation != op || s->fd != fd) {
                fprintf(stderr,
                        "step %u: expected op=%d fd=%d, got op=%d fd=%d\n",
                        cursor - 1, s->operation, s->fd, op, fd);
                assert(false);
        }
        errno = s->error;
        return s;
}

static struct step *event(int fd, uint32_t mask) {
        struct step *s = add(OP_WAIT, POLLER, 1, 0);
        s->events[0] = (struct event_spec){.fd = fd, .mask = mask};
        return s;
}

static void io(enum operation op, int fd, const char *bytes, size_t length,
               int result, int error) {
        struct step *s = add(op, fd, result, error);
        s->bytes = bytes;
        s->length = length;
}

static void accept_one(int fd) {
        event(LISTENER, NEPOLL_ACCEPT);
        add(OP_ACCEPT, LISTENER, fd, 0);
        add(OP_ACCEPT, LISTENER, -1, EAGAIN);
}

static void echo(int fd, const char *bytes, size_t length) {
        event(fd, NEPOLL_READ);
        io(OP_RECV, fd, bytes, length, (int)length, 0);
        io(OP_SEND, fd, bytes, length, (int)length, 0);
}

static void reset(void) {
        length = cursor = ctl_calls = fail_ctl = 0;
        fault = NONE;
        memset(opened, 0, sizeof(opened));
        memset(registered, 0, sizeof(registered));
        memset(subscriptions, 0, sizeof(subscriptions));
        memset(retired, 0, sizeof(retired));
        poller_open = false;
}

static void run(void) {
        assert(tcp_echo_server_entry(NULL) == -1);
        assert(errno == EIO);
        assert(cursor == length);
        assert(!poller_open);
        for (unsigned i = 0; i < MAX_FD; i++)
                assert(!opened[i] && !registered[i]);
}

static void finish(void) {
        add(OP_WAIT, POLLER, -1, EIO);
        run();
}

int __wrap_nsocket(int domain, int type, int protocol) {
        assert(domain == AF_INET && type == (SOCK_STREAM | SOCK_NONBLOCK));
        assert(protocol == 0);
        if (fault == SOCKET_FAIL) {
                errno = EIO;
                return -1;
        }
        opened[LISTENER] = true;
        return LISTENER;
}

int __wrap_nepoll_create(void) {
        assert(opened[LISTENER]);
        if (fault == POLLER_FAIL) {
                errno = EIO;
                return -1;
        }
        poller_open = true;
        return POLLER;
}

int __wrap_nbind(int fd, const struct sockaddr *addr, socklen_t size) {
        assert(fd == LISTENER && addr && size == sizeof(struct sockaddr_in));
        errno = EIO;
        return fault == BIND_FAIL ? -1 : 0;
}

int __wrap_nlisten(int fd, int backlog) {
        assert(fd == LISTENER && backlog == 16);
        errno = EIO;
        return fault == LISTEN_FAIL ? -1 : 0;
}

int __wrap_nepoll_ctl(int poller, int op, int fd,
                      const struct nepoll_event *ev) {
        assert(poller == POLLER && poller_open && opened[fd]);
        if (op == NEPOLL_CTL_DEL) {
                retired[fd] = subscriptions[fd];
                registered[fd] = false;
                return 0;
        }
        assert(op == NEPOLL_CTL_ADD || op == NEPOLL_CTL_MOD);
        assert(registered[fd] == (op == NEPOLL_CTL_MOD));
        assert(ev &&
               (ev->events == (fd == LISTENER ? NEPOLL_ACCEPT : NEPOLL_READ) ||
                (fd != LISTENER && ev->events == NEPOLL_WRITE)));
        if (++ctl_calls == fail_ctl) {
                errno = EIO;
                return -1;
        }
        registered[fd] = true;
        subscriptions[fd] = *ev;
        subscriptions[fd].fd = fd;
        return 0;
}

int __wrap_nepoll_wait(int poller, struct nepoll_event *events, int maxevents,
                       int timeout) {
        assert(poller_open && timeout == -1);
        struct step *s = next(OP_WAIT, poller);
        assert(s->result <= maxevents);
        memset(reads, 0, sizeof(reads));
        memset(writes, 0, sizeof(writes));
        accepts = 0;
        for (int i = 0; i < s->result; i++) {
                struct event_spec *spec = &s->events[i];
                int fd = spec->fd;
                assert(registered[fd] || spec->stale);
                events[i] = spec->stale ? retired[fd] : subscriptions[fd];
                assert(spec->stale ||
                       (spec->mask &
                        (events[i].events | NEPOLL_HUP | NEPOLL_ERROR)));
                events[i].events = spec->mask;
        }
        return s->result;
}

int __wrap_naccept4(int fd, struct sockaddr *addr, socklen_t *size, int flags) {
        assert(fd == LISTENER && !addr && !size && flags == SOCK_NONBLOCK);
        assert(++accepts <= LIMIT);
        struct step *s = next(OP_ACCEPT, fd);
        if (s->result >= 0) {
                assert(s->result < MAX_FD && !opened[s->result]);
                opened[s->result] = true;
        }
        return s->result;
}

ssize_t __wrap_nrecv(int fd, void *buf, size_t size, int flags) {
        assert(opened[fd] && registered[fd] && flags == 0 && size == BUFFER);
        assert(subscriptions[fd].events == NEPOLL_READ && ++reads[fd] == 1);
        struct step *s = next(OP_RECV, fd);
        if (s->result > 0) {
                assert((size_t)s->result == s->length && s->length <= size);
                memcpy(buf, s->bytes, s->length);
        }
        return s->result;
}

ssize_t __wrap_nsend(int fd, const void *buf, size_t size, int flags) {
        assert(opened[fd] && registered[fd] && flags == 0 && ++writes[fd] == 1);
        struct step *s = next(OP_SEND, fd);
        assert(size == s->length &&
               (s->result < 0 || (size_t)s->result <= size));
        assert(memcmp(buf, s->bytes, size) == 0);
        return s->result;
}

int __wrap_nclose(int fd) {
        assert(opened[fd] && !registered[fd]);
        opened[fd] = false;
        return 0;
}

int __wrap_nepoll_close(int poller) {
        assert(poller == POLLER && poller_open);
        for (unsigned i = 0; i < MAX_FD; i++)
                assert(!opened[i] && !registered[i]);
        poller_open = false;
        return 0;
}

static void concurrency(void) {
        reset();
        accept_one(2);
        echo(2, "hello", 5);
        /* A stays connected and idle; B is accepted and echoes independently.
         */
        accept_one(3);
        echo(3, "B", 1);
        event(2, NEPOLL_READ);
        io(OP_RECV, 2, NULL, 0, -1, EAGAIN);
        event(2, NEPOLL_READ);
        io(OP_RECV, 2, "abcdef", 6, 6, 0);
        io(OP_SEND, 2, "abcdef", 6, 2, 0);
        struct step *s = event(2, NEPOLL_WRITE);
        s->result = 2;
        s->events[1] = (struct event_spec){.fd = 3, .mask = NEPOLL_READ};
        io(OP_SEND, 2, "cdef", 4, -1, EAGAIN);
        io(OP_RECV, 3, "B again", 7, 7, 0);
        io(OP_SEND, 3, "B again", 7, 7, 0);
        event(2, NEPOLL_WRITE);
        io(OP_SEND, 2, "cdef", 4, 4, 0);
        echo(2, "resumed", 7);
        finish();
}

static void half_close_and_reuse(void) {
        char payload[BUFFER];
        for (unsigned i = 0; i < sizeof(payload); i++)
                payload[i] = (char)i;
        reset();
        accept_one(2);
        /* FIN accompanies >1280 bytes; send all bytes before acknowledging EOF.
         */
        event(2, NEPOLL_READ | NEPOLL_HUP);
        io(OP_RECV, 2, payload, sizeof(payload), sizeof(payload), 0);
        io(OP_SEND, 2, payload, sizeof(payload), 17, 0);
        event(2, NEPOLL_WRITE | NEPOLL_HUP);
        io(OP_SEND, 2, payload + 17, sizeof(payload) - 17, -1, EAGAIN);
        event(2, NEPOLL_WRITE | NEPOLL_HUP);
        io(OP_SEND, 2, payload + 17, sizeof(payload) - 17, sizeof(payload) - 17,
           0);
        event(2, NEPOLL_READ | NEPOLL_HUP);
        io(OP_RECV, 2, "tail", 4, 4, 0);
        io(OP_SEND, 2, "tail", 4, 4, 0);
        event(2, NEPOLL_HUP);
        io(OP_RECV, 2, NULL, 0, 0, 0);
        /* Reuse the fd and slot before dispatching an already-returned event.
         */
        struct step *s = event(LISTENER, NEPOLL_ACCEPT);
        s->result = 2;
        s->events[1] =
            (struct event_spec){.fd = 2, .mask = NEPOLL_READ, .stale = true};
        add(OP_ACCEPT, LISTENER, 2, 0);
        add(OP_ACCEPT, LISTENER, -1, EAGAIN);
        echo(2, "new", 3);
        finish();
}

static void capacity_and_errors(void) {
        reset();
        event(LISTENER, NEPOLL_ACCEPT);
        for (int fd = 2; fd < 2 + LIMIT; fd++)
                add(OP_ACCEPT, LISTENER, fd, 0);
        /* Flood accepts are bounded even while the table is full. */
        struct step *s = event(LISTENER, NEPOLL_ACCEPT);
        s->result = 2;
        s->events[1] = (struct event_spec){.fd = 3, .mask = NEPOLL_READ};
        for (int i = 0; i < LIMIT; i++)
                add(OP_ACCEPT, LISTENER, 34, 0);
        io(OP_RECV, 3, "live", 4, 4, 0);
        io(OP_SEND, 3, "live", 4, 4, 0);
        event(2, NEPOLL_ERROR | NEPOLL_HUP);
        io(OP_RECV, 2, NULL, 0, -1, ECONNRESET);
        accept_one(34);
        echo(34, "space", 5);
        /* Send failures, including an unexpected zero, close only that peer. */
        event(34, NEPOLL_READ);
        io(OP_RECV, 34, "x", 1, 1, 0);
        io(OP_SEND, 34, "x", 1, 0, 0);
        accept_one(34);
        event(34, NEPOLL_READ);
        io(OP_RECV, 34, "x", 1, 1, 0);
        io(OP_SEND, 34, "x", 1, -1, ECONNRESET);
        echo(3, "still live", 10);
        finish();
}

static void failures(void) {
        for (int f = SOCKET_FAIL; f <= LISTEN_FAIL; f++) {
                reset();
                fault = f;
                run();
        }
        reset();
        fail_ctl = 1; /* Listener registration. */
        run();
        reset();
        fail_ctl = 2; /* Accepted fd must close even if ADD fails. */
        event(LISTENER, NEPOLL_ACCEPT);
        add(OP_ACCEPT, LISTENER, 2, 0);
        run();
        reset();
        fail_ctl = 3; /* MOD failure after an echo. */
        accept_one(2);
        echo(2, "x", 1);
        run();
        reset();
        event(LISTENER, NEPOLL_ACCEPT);
        add(OP_ACCEPT, LISTENER, -1, EIO);
        run();
        reset();
        accept_one(2);
        event(LISTENER, NEPOLL_ERROR);
        run();
        reset();
        add(OP_WAIT, POLLER, -1, EINTR);
        event(LISTENER, NEPOLL_ACCEPT);
        add(OP_ACCEPT, LISTENER, -1, EINTR);
        add(OP_ACCEPT, LISTENER, -1, ECONNABORTED);
        add(OP_ACCEPT, LISTENER, 2, 0);
        add(OP_ACCEPT, LISTENER, -1, EAGAIN);
        event(2, NEPOLL_READ);
        io(OP_RECV, 2, NULL, 0, -1, EINTR);
        event(2, NEPOLL_READ);
        io(OP_RECV, 2, "x", 1, 1, 0);
        io(OP_SEND, 2, "x", 1, -1, EINTR);
        event(2, NEPOLL_WRITE);
        io(OP_SEND, 2, "x", 1, 1, 0);
        finish();
}

int main(void) {
        concurrency();
        half_close_and_reuse();
        capacity_and_errors();
        failures();
        puts(
            "PASS: TCP echo concurrency, backpressure, EOF, reuse and cleanup");
        return 0;
}
