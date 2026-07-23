#include "net_addr.h"

struct local_addr *g_local_addr = NULL;

int get_fd_from_bitmap(void) {
        int fd = DEFAULT_FD_NUM;
        return fd;
}

struct local_addr *get_local_addr_from_fd(int sockfd) {
        struct local_addr *addr;

        for (addr = g_local_addr; addr != NULL; addr = addr->next) {
                if (addr->fd_ == sockfd)
                        return addr;
        }

        return NULL;
}

struct local_addr *get_local_addr_from_ip_port(uint32_t ip, uint16_t port,
                                               uint8_t protocol) {
        struct local_addr *addr;

        for (addr = g_local_addr; addr != NULL; addr = addr->next) {
                if (addr->local_ip_ == ip && addr->local_port_ == port &&
                    addr->protocol_ == protocol)
                        return addr;
        }

        return NULL;
}
