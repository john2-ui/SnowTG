/**
 * @file socket_api.h
 * @author your name (you@domain.com)
 * @brief
 * @version 0.1
 * @date 2026-07-23
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef UDP_SOCKET_API_H
#define UDP_SOCKET_API_H

#include "net_addr.h"
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
// enum SOCKET_TYPE { SOCK_DGRAM = 1, SOCK_STREAM = 2 };

int nsocket(__attribute__((unused)) int domain, int type,
            __attribute__((unused)) int protocol);

int nbind(int sockfd, const struct sockaddr *addr,
          __attribute__((unused)) socklen_t addrlen);

ssize_t nrecvfrom(int sockfd, void *buf, size_t len,
                  __attribute__((unused)) int flags, struct sockaddr *src_addr,
                  __attribute__((unused)) socklen_t *addrlen);

ssize_t nsendto(int sockfd, const void *buf, size_t len,
                __attribute__((unused)) int flags,
                const struct sockaddr *dest_addr,
                __attribute__((unused)) socklen_t addrlen);

int nclose(int sockfd);

#endif