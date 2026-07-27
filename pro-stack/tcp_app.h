/**
 * @file tcp_app.h
 * @brief Entry points for the example TCP server and client applications.
 */
#ifndef NETARCH_TCP_APP_H
#define NETARCH_TCP_APP_H

/**
 * Blocking TCP echo server: nsocket/nbind/nlisten/naccept, then
 * nrecv/nsend until the peer stops, then nclose on the accepted fd.
 *
 * @param arg Reserved launch argument; currently unused.
 * @return -1 if listen-socket setup fails. The accept loop does not return.
 */
int tcp_server_entry(void *arg);

/**
 * Blocking TCP client used to drive active open: nsocket/nbind/nconnect,
 * then nsend/nrecv/nclose in a loop.
 *
 * Requires tcp_ops.connect (and later active close) in the stack.
 *
 * @param arg Reserved launch argument; currently unused.
 * @return -1 if the client socket cannot be created. The connect loop does
 *         not return on success.
 */
int tcp_client_entry(void *arg);

#endif /* NETARCH_TCP_APP_H */
