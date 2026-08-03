#ifndef DPDK_L_TCP_ECHO_H
#define DPDK_L_TCP_ECHO_H

/** Run the blocking BSD-style TCP echo server on an application lcore. */
int tcp_echo_server_entry(void *arg);

/** Run the blocking BSD-style TCP active-open echo client. */
int tcp_echo_client_entry(void *arg);

#endif /* DPDK_L_TCP_ECHO_H */
