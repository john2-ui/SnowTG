/**
 * @file tcp_app.h
 * @brief Entry point for the example TCP echo application.
 */
#ifndef NETARCH_TCP_APP_H
#define NETARCH_TCP_APP_H

/**
 * Run a blocking TCP echo server on an application lcore.
 *
 * Listens, accepts one connection at a time, and echoes bytes with
 * nrecvfrom/nsendto until the peer stops sending.
 *
 * @param arg Reserved launch argument; currently unused.
 * @return -1 if socket setup fails. The normal accept/echo loop does not
 *         return.
 */
int tcp_app_entry(void *arg);

#endif /* NETARCH_TCP_APP_H */
