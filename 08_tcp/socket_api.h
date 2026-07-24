/**
 * @file socket_api.h
 * @brief Compatibility shim that re-exports the unified socket API.
 *
 * The socket API lives in socket.h now. This header is kept so existing
 * applications (e.g. udp_app.c) build unchanged; new code should include
 * socket.h directly.
 */
#ifndef NETARCH_SOCKET_API_H
#define NETARCH_SOCKET_API_H

#include "socket.h"

#endif /* NETARCH_SOCKET_API_H */
