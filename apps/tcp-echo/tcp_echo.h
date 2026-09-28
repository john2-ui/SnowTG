#ifndef DPDK_L_TCP_ECHO_H
#define DPDK_L_TCP_ECHO_H

/** Run a nonblocking, nepoll-driven TCP echo server on an application lcore.
 * At most 32 clients, each with a bounded 1280-byte echo buffer. */
int tcp_echo_server_entry(void *arg);

/** Run the blocking BSD-style TCP active-open echo client. */
int tcp_echo_client_entry(void *arg);

#endif /* DPDK_L_TCP_ECHO_H */
