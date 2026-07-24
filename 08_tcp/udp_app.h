/**
 * @file udp_app.h
 * @brief Entry point for the example UDP echo application.
 */
#ifndef NETARCH_UDP_APP_H
#define NETARCH_UDP_APP_H

/**
 * Run the blocking UDP echo loop on an application lcore.
 *
 * @param arg Reserved launch argument; currently unused.
 * @return -1 if socket setup fails. The normal echo loop does not return.
 */
int udp_app_entry(void *arg);

#endif /* NETARCH_UDP_APP_H */
