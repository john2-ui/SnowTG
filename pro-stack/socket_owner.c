#include "socket_owner.h"

#include "config.h"
#include "socket.h"
#include "tcp.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <rte_lcore.h>
#include <rte_pause.h>
#include <rte_ring.h>
#include <string.h>

/*
 * The current program has one packet worker, hence one owner context.  The
 * handle already carries owner_lcore so this can later become an array indexed
 * by RSS queue/worker without changing the application ABI.
 */
static struct socket_owner g_owner;
static bool g_owner_ready;

/** Resolve a handle only on the owner lcore; no pointer crosses this boundary.
 */
static struct nsock *owner_lookup(struct nsock_handle handle) {
        struct nsock *sk;

        if (!g_owner_ready || handle.id >= NSOCK_ID_MAX ||
            handle.owner_lcore != g_owner.lcore_id)
                return NULL;

        sk = g_owner.slots[handle.id];
        if (sk == NULL || sk->generation != handle.generation ||
            sk->protocol != handle.protocol)
                return NULL;

        return sk;
}

/** Append a command to an owner-only FIFO wait queue. */
static void waitq_push(struct sock_cmd **head, struct sock_cmd **tail,
                       struct sock_cmd *cmd) {
        cmd->next = NULL;
        if (*tail != NULL)
                (*tail)->next = cmd;
        else
                *head = cmd;
        *tail = cmd;
}

/** Remove and return the first command from an owner-only wait queue. */
static struct sock_cmd *waitq_pop(struct sock_cmd **head,
                                  struct sock_cmd **tail) {
        struct sock_cmd *cmd = *head;
        if (cmd == NULL)
                return NULL;
        *head = cmd->next;
        if (*head == NULL)
                *tail = NULL;
        cmd->next = NULL;
        return cmd;
}

/** Restore a command at the front when a nonblocking owner probe hits EAGAIN.
 */
static void waitq_push_front(struct sock_cmd **head, struct sock_cmd **tail,
                             struct sock_cmd *cmd) {
        cmd->next = *head;
        *head = cmd;
        if (*tail == NULL)
                *tail = cmd;
}

int socket_owner_init(unsigned int lcore_id) {
        memset(&g_owner, 0, sizeof(g_owner));
        g_owner.lcore_id = lcore_id;

        /*
         * Every application lcore may submit commands, while exactly one
         * packet worker consumes them.  RING_F_SC_DEQ encodes only the latter;
         * enqueue therefore retains DPDK's multi-producer synchronization.
         */
        g_owner.command_ring =
            rte_ring_create("socket_commands", NSOCK_REGISTRY_ENTRIES,
                            rte_socket_id(), RING_F_SC_DEQ);
        if (g_owner.command_ring == NULL)
                return -1;

        g_owner_ready = true;
        return 0;
}

int socket_owner_adopt(struct nsock *sk) {
        if (!g_owner_ready || sk == NULL)
                return -EINVAL;
        if (rte_lcore_id() != g_owner.lcore_id)
                return -EPERM;

        for (uint32_t id = 0; id < NSOCK_ID_MAX; id++) {
                if (g_owner.slots[id] != NULL)
                        continue;

                /*
                 * Retirement, rather than allocation, advances generation.
                 * Thus every handle becomes stale at the exact point its
                 * object is unpublished; a reused slot receives the already
                 * advanced generation.
                 */
                uint32_t generation = g_owner.generations[id];
                if (generation == 0) {
                        generation = 1;
                        g_owner.generations[id] = generation;
                }

                sk->id = id;
                sk->generation = generation;
                sk->owner_lcore = (uint16_t)g_owner.lcore_id;
                g_owner.slots[id] = sk;
                return 0;
        }

        return -ENFILE;
}

void socket_owner_retire(struct nsock *sk) {
        if (sk == NULL || sk->id >= NSOCK_ID_MAX)
                return;

        /*
         * Complete parked stack-resident commands before making the socket
         * unreachable.  Otherwise their application threads would wait
         * forever and their command storage could never be reclaimed.
         */
        socket_owner_abort_waiters(sk, ECANCELED);

        if (g_owner.slots[sk->id] == sk) {
                g_owner.slots[sk->id] = NULL;
                uint32_t next = g_owner.generations[sk->id] + 1;
                /* Generation zero remains reserved after uint32_t wrap. */
                g_owner.generations[sk->id] = next == 0 ? 1 : next;
        }
}

struct nsock_handle socket_owner_handle(const struct nsock *sk) {
        struct nsock_handle handle = {
            .id = NSOCK_INVALID_ID,
        };
        if (sk == NULL)
                return handle;

        handle.id = sk->id;
        handle.generation = sk->generation;
        handle.owner_lcore = sk->owner_lcore;
        handle.protocol = sk->protocol;
        return handle;
}

void socket_owner_complete(struct sock_cmd *cmd, ssize_t result, int error) {
        pthread_mutex_lock(&cmd->done_mutex);
        cmd->result = result;
        cmd->error = error;
        cmd->done = true;
        pthread_cond_signal(&cmd->done_cond);
        pthread_mutex_unlock(&cmd->done_mutex);
}

int socket_owner_call(struct sock_cmd *cmd) {
        if (!g_owner_ready) {
                errno = ENETDOWN;
                return -1;
        }
        if (pthread_mutex_init(&cmd->done_mutex, NULL) != 0) {
                errno = ENOMEM;
                return -1;
        }
        if (pthread_cond_init(&cmd->done_cond, NULL) != 0) {
                pthread_mutex_destroy(&cmd->done_mutex);
                errno = ENOMEM;
                return -1;
        }
        cmd->done = false;
        cmd->next = NULL;

        /*
         * Apply backpressure to command producers instead of returning
         * ENOBUFS.  In particular, nclose has already detached its fd before
         * submitting CLOSE; dropping that command would leave an unreachable
         * live TCB.  Waiting for ring space is consistent with the synchronous
         * API, which already waits for owner completion.
         */
        while (rte_ring_mp_enqueue(g_owner.command_ring, cmd) != 0)
                rte_pause();

        /*
         * Waiting here blocks only the application lcore.  A command that
         * cannot yet progress is retained by the owner, whose loop continues
         * to process packets and timers until it can complete the request.
         */
        pthread_mutex_lock(&cmd->done_mutex);
        while (!cmd->done)
                pthread_cond_wait(&cmd->done_cond, &cmd->done_mutex);
        pthread_mutex_unlock(&cmd->done_mutex);

        pthread_cond_destroy(&cmd->done_cond);
        pthread_mutex_destroy(&cmd->done_mutex);
        if (cmd->result < 0)
                errno = cmd->error;

        return cmd->result < 0 ? -1 : 0;
}

/** Convert legacy transport "-1 plus errno" into one command completion. */
static void complete_transport_result(struct sock_cmd *cmd, ssize_t result) {
        int error = result < 0 ? errno : 0;
        if (result < 0 && error == 0)
                error = EIO;
        socket_owner_complete(cmd, result, error);
}

void socket_owner_wake_recv(struct nsock *sk) {
        while (sk != NULL && sk->recv_wait_head != NULL) {
                /*
                 * Remove before probing. tcp_recv may drain OFO data, whose
                 * delivery recursively wakes anothebr waiter; keeping the
                 * current command linked would execute it twice.
                 */
                struct sock_cmd *cmd =
                    waitq_pop(&sk->recv_wait_head, &sk->recv_wait_tail);
                ssize_t result;

                errno = 0;
                if (cmd->type == SOCK_CMD_RECV) {
                        result =
                            sk->ops->recv(sk, cmd->args.io.buf,
                                          cmd->args.io.len, cmd->args.io.flags);
                } else {
                        result = sk->ops->recvfrom(
                            sk, cmd->args.io.buf, cmd->args.io.len,
                            cmd->args.io.flags, cmd->args.io.out_addr,
                            cmd->args.io.out_addrlen);
                }

                if (result < 0 && errno == EAGAIN &&
                    !(cmd->args.io.flags & MSG_DONTWAIT)) {
                        waitq_push_front(&sk->recv_wait_head,
                                         &sk->recv_wait_tail, cmd);
                        return;
                }

                complete_transport_result(cmd, result);
        }
}

void socket_owner_wake_send(struct nsock *sk) {
        while (sk != NULL && sk->send_wait_head != NULL) {
                struct sock_cmd *cmd = sk->send_wait_head;

                errno = 0;
                ssize_t result = sk->ops->send(
                    sk, cmd->args.io.buf, cmd->args.io.len, cmd->args.io.flags);
                if (result < 0 && errno == EAGAIN)
                        return;

                (void)waitq_pop(&sk->send_wait_head, &sk->send_wait_tail);
                complete_transport_result(cmd, result);
        }
}

void socket_owner_wake_accept(struct nsock *listener) {
        while (listener != NULL && listener->accept_wait_head != NULL) {
                struct nsock *child = tcp_accept_owned(listener);
                if (child == NULL) {
                        if (errno == EAGAIN)
                                return;

                        struct sock_cmd *failed =
                            waitq_pop(&listener->accept_wait_head,
                                      &listener->accept_wait_tail);
                        complete_transport_result(failed, -1);
                        continue;
                }

                struct sock_cmd *cmd = waitq_pop(&listener->accept_wait_head,
                                                 &listener->accept_wait_tail);
                child->app_visible = true;
                cmd->result_handle = socket_owner_handle(child);
                if (cmd->args.address.out_addr != NULL) {
                        struct sockaddr_in *sin =
                            (struct sockaddr_in *)cmd->args.address.out_addr;
                        sin->sin_family = AF_INET;
                        sin->sin_port = child->u.tcp.remote_port;
                        sin->sin_addr.s_addr = child->u.tcp.remote_ip;
                        if (cmd->args.address.out_addrlen != NULL)
                                *cmd->args.address.out_addrlen = sizeof(*sin);
                }
                socket_owner_complete(cmd, 0, 0);
        }
}

void socket_owner_complete_connect(struct nsock *sk, int error) {
        if (sk == NULL || sk->connect_waiter == NULL)
                return;
        struct sock_cmd *cmd = sk->connect_waiter;
        sk->connect_waiter = NULL;
        socket_owner_complete(cmd, error == 0 ? 0 : -1, error);
}

void socket_owner_abort_waiters(struct nsock *sk, int error) {
        struct sock_cmd *cmd;
        if (sk == NULL)
                return;

        if (sk->connect_waiter != NULL) {
                cmd = sk->connect_waiter;
                sk->connect_waiter = NULL;
                socket_owner_complete(cmd, -1, error);
        }

        while ((cmd = waitq_pop(&sk->recv_wait_head, &sk->recv_wait_tail)) !=
               NULL)
                socket_owner_complete(cmd, -1, error);
        while ((cmd = waitq_pop(&sk->send_wait_head, &sk->send_wait_tail)) !=
               NULL)
                socket_owner_complete(cmd, -1, error);
        while ((cmd = waitq_pop(&sk->accept_wait_head,
                                &sk->accept_wait_tail)) != NULL)
                socket_owner_complete(cmd, -1, error);
}

/** Handle one request without ever sleeping on protocol progress. */
static void owner_process_one(struct sock_cmd *cmd) {
        struct nsock *sk = NULL;
        ssize_t result;

        if (cmd->type == SOCK_CMD_CREATE) {
                sk = nsock_alloc(-1, (uint8_t)cmd->args.create.protocol);
                if (sk == NULL || socket_owner_adopt(sk) != 0) {
                        if (sk != NULL)
                                nsock_free(sk);
                        socket_owner_complete(cmd, -1, ENOMEM);
                        return;
                }
                sk->app_visible = true;
                cmd->result_handle = socket_owner_handle(sk);
                socket_owner_complete(cmd, 0, 0);
                return;
        }

        sk = owner_lookup(cmd->handle);
        if (sk == NULL) {
                socket_owner_complete(cmd, -1, EBADF);
                return;
        }
        if (sk->app_closed && cmd->type != SOCK_CMD_CLOSE) {
                socket_owner_complete(cmd, -1, EBADF);
                return;
        }

        errno = 0;
        switch (cmd->type) {
        case SOCK_CMD_BIND: {
                const struct sockaddr_in *sin =
                    (const struct sockaddr_in *)&cmd->args.address.addr;
                result =
                    nsock_bind_local(sk, sin->sin_addr.s_addr, sin->sin_port);
                if (result < 0) {
                        errno = -result;
                        result = -1;
                }
                complete_transport_result(cmd, result);
                return;
        }
        case SOCK_CMD_CONNECT:
                if (sk->ops->connect == NULL || sk->connect_waiter != NULL) {
                        socket_owner_complete(cmd, -1,
                                              sk->connect_waiter ? EALREADY
                                                                 : EOPNOTSUPP);
                        return;
                }
                result = sk->ops->connect(
                    sk, (const struct sockaddr *)&cmd->args.address.addr,
                    cmd->args.address.addrlen);
                if (result < 0 && errno == EINPROGRESS) {
                        sk->connect_waiter = cmd;
                        return;
                }
                complete_transport_result(cmd, result);
                return;
        case SOCK_CMD_LISTEN:
                if (sk->ops->listen == NULL) {
                        socket_owner_complete(cmd, -1, EOPNOTSUPP);
                        return;
                }
                complete_transport_result(
                    cmd, sk->ops->listen(sk, cmd->args.listen.backlog));
                return;
        case SOCK_CMD_ACCEPT:
                if (sk->ops->accept == NULL) {
                        socket_owner_complete(cmd, -1, EOPNOTSUPP);
                        return;
                }
                waitq_push(&sk->accept_wait_head, &sk->accept_wait_tail, cmd);
                socket_owner_wake_accept(sk);
                return;
        case SOCK_CMD_SEND:
                if (sk->ops->send == NULL) {
                        socket_owner_complete(cmd, -1, EOPNOTSUPP);
                        return;
                }
                result = sk->ops->send(sk, cmd->args.io.buf, cmd->args.io.len,
                                       cmd->args.io.flags);
                if (result < 0 && errno == EAGAIN &&
                    !(cmd->args.io.flags & MSG_DONTWAIT)) {
                        waitq_push(&sk->send_wait_head, &sk->send_wait_tail,
                                   cmd);
                        return;
                }
                complete_transport_result(cmd, result);
                return;
        case SOCK_CMD_RECV:
        case SOCK_CMD_RECVFROM:
                if ((cmd->type == SOCK_CMD_RECV && sk->ops->recv == NULL) ||
                    (cmd->type == SOCK_CMD_RECVFROM &&
                     sk->ops->recvfrom == NULL)) {
                        socket_owner_complete(cmd, -1, EOPNOTSUPP);
                        return;
                }
                waitq_push(&sk->recv_wait_head, &sk->recv_wait_tail, cmd);
                socket_owner_wake_recv(sk);
                return;
        case SOCK_CMD_SENDTO:
                if (sk->ops->sendto == NULL) {
                        socket_owner_complete(cmd, -1, EOPNOTSUPP);
                        return;
                }
                complete_transport_result(
                    cmd,
                    sk->ops->sendto(sk, cmd->args.io.buf, cmd->args.io.len,
                                    cmd->args.io.flags,
                                    (const struct sockaddr *)&cmd->args.io.addr,
                                    cmd->args.io.addrlen));
                return;
        case SOCK_CMD_CLOSE:
                if (sk->app_closed) {
                        socket_owner_complete(cmd, -1, EBADF);
                        return;
                }
                sk->app_closed = true;
                socket_owner_abort_waiters(sk, ECANCELED);
                if (sk->ops->close == NULL) {
                        socket_owner_complete(cmd, -1, EOPNOTSUPP);
                        return;
                }
                /*
                 * close() is an ownership transfer, not a wait for TIME_WAIT.
                 * The transport starts teardown and may immediately destroy
                 * UDP/listener sockets; do not dereference sk afterwards.
                 */
                result = sk->ops->close(sk);
                complete_transport_result(cmd, result);
                return;
        default:
                socket_owner_complete(cmd, -1, EINVAL);
                return;
        }
}

void socket_owner_process_commands(void) {
        struct sock_cmd *commands[BURST_SIZE];
        unsigned int count = rte_ring_sc_dequeue_burst(
            g_owner.command_ring, (void **)commands, BURST_SIZE, NULL);

        for (unsigned int i = 0; i < count; i++)
                owner_process_one(commands[i]);
}