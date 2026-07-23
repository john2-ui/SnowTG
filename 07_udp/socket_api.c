#include "socket_api.h"

#include "arp.h"
#include "config.h"
#include "list.h"
#include "log.h"
#include "net_addr.h"
#include "net_context.h"
#include "udp.h"

#include <netinet/in.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_udp.h>
#include <string.h>

/** Release every packet still owned by a socket ring. */
static void free_queued_packets(struct rte_ring *ring) {
        struct rte_mbuf *mbuf;

        while (rte_ring_sc_dequeue(ring, (void **)&mbuf) == 0)
                rte_pktmbuf_free(mbuf);
}

int nsocket(__attribute__((unused)) int domain, int type,
            __attribute__((unused)) int protocol) {
        int fd = get_fd_from_bitmap();

        struct local_addr *addr =
            rte_malloc("local_addr", sizeof(struct local_addr), 0);
        if (addr == NULL) {
                LOG_ERROR("rte_malloc() failed");
                return -1;
        }

        memset(addr, 0, sizeof(*addr));
        addr->fd_ = fd;

        if (type == SOCK_DGRAM)
                addr->protocol_ = IPPROTO_UDP;

        addr->recv_buf_ =
            rte_ring_create("recv_buf", RING_SIZE, rte_socket_id(),
                            RING_F_SP_ENQ | RING_F_SC_DEQ);
        if (addr->recv_buf_ == NULL) {
                LOG_ERROR("rte_ring_create() failed");
                rte_free(addr);
                return -1;
        }

        addr->send_buf_ = rte_ring_create("send_buf", RING_SIZE,
                                          rte_socket_id(), RING_F_SC_DEQ);
        if (addr->send_buf_ == NULL) {
                LOG_ERROR("rte_ring_create() failed");
                rte_ring_free(addr->recv_buf_);
                rte_free(addr);
                return -1;
        }

        if (pthread_mutex_init(&addr->mutex_, NULL) != 0) {
                LOG_ERROR("pthread_mutex_init() failed");
                rte_ring_free(addr->send_buf_);
                rte_ring_free(addr->recv_buf_);
                rte_free(addr);
                return -1;
        }
        if (pthread_cond_init(&addr->cond_, NULL) != 0) {
                LOG_ERROR("pthread_cond_init() failed");
                pthread_mutex_destroy(&addr->mutex_);
                rte_ring_free(addr->send_buf_);
                rte_ring_free(addr->recv_buf_);
                rte_free(addr);
                return -1;
        }

        LL_ADD(addr, g_local_addr);

        return fd;
}

int nbind(int sockfd, const struct sockaddr *addr,
          __attribute__((unused)) socklen_t addrlen) {
        struct local_addr *host = get_local_addr_from_fd(sockfd);
        if (host == NULL) {
                LOG_ERROR("get_local_addr_from_fd() failed");
                return -1;
        }

        const struct sockaddr_in *laddr = (const struct sockaddr_in *)addr;
        host->local_port_ = laddr->sin_port;
        host->local_ip_ = laddr->sin_addr.s_addr;
        rte_memcpy(host->local_mac, g_net.local_mac, RTE_ETHER_ADDR_LEN);

        LOG_INFO("nbind fd=%d " IP_FMT ":%u proto=%u", sockfd,
                 IP_ARG(host->local_ip_), rte_be_to_cpu_16(host->local_port_),
                 host->protocol_);

        return 0;
}

ssize_t nrecvfrom(int sockfd, void *buf, size_t len,
                  __attribute__((unused)) int flags, struct sockaddr *src_addr,
                  __attribute__((unused)) socklen_t *addrlen) {
        struct local_addr *host = get_local_addr_from_fd(sockfd);
        if (host == NULL) {
                LOG_ERROR("get_local_addr_from_fd() failed");
                return -1;
        }

        struct rte_mbuf *mbuf;

        int nb = -1;
        pthread_mutex_lock(&host->mutex_);
        while ((nb = rte_ring_sc_dequeue(host->recv_buf_, (void **)&mbuf)) !=
               0) {
                pthread_cond_wait(&host->cond_, &host->mutex_);
        }
        pthread_mutex_unlock(&host->mutex_);

        LOG_INFO("nrecvfrom fd=%d dequeued mbuf", sockfd);

        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);

        struct sockaddr_in *sin = (struct sockaddr_in *)src_addr;
        sin->sin_family = AF_INET;
        sin->sin_port = udp->src_port;
        sin->sin_addr.s_addr = ip->src_addr;

        size_t payload_len =
            rte_be_to_cpu_16(udp->dgram_len) - sizeof(struct rte_udp_hdr);
        void *data = (void *)(udp + 1);

        if (len < payload_len) {
                rte_memcpy(buf, data, len);
                memmove(data, (uint8_t *)data + len, payload_len - len);
                udp->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) +
                                                  payload_len - len);

                if (rte_ring_sp_enqueue(host->recv_buf_, mbuf) != 0) {
                        LOG_ERROR("recv_buf full while truncating, dropping");
                        rte_pktmbuf_free(mbuf);
                }
                return (ssize_t)len;
        }

        rte_memcpy(buf, data, payload_len);
        rte_pktmbuf_free(mbuf);
        return (ssize_t)payload_len;
}

ssize_t nsendto(int sockfd, const void *buf, size_t len,
                __attribute__((unused)) int flags,
                const struct sockaddr *dest_addr,
                __attribute__((unused)) socklen_t addrlen) {
        struct local_addr *host = get_local_addr_from_fd(sockfd);
        if (host == NULL) {
                LOG_ERROR("get_local_addr_from_fd() failed");
                return -1;
        }

        if (g_net.mp == NULL) {
                LOG_ERROR("mbuf pool not initialized");
                return -1;
        }

        const struct sockaddr_in *daddr = (const struct sockaddr_in *)dest_addr;
        const uint8_t *dst_mac = arp_lookup(daddr->sin_addr.s_addr);
        if (dst_mac == NULL)
                dst_mac = g_broadcast_mac;

        struct rte_mbuf *mbuf = udp_build_pkt(
            g_net.mp, dst_mac, host->local_ip_, daddr->sin_addr.s_addr,
            host->local_port_, daddr->sin_port, buf, (uint16_t)len);
        if (mbuf == NULL)
                return -1;

        if (rte_ring_mp_enqueue(host->send_buf_, mbuf) != 0) {
                LOG_ERROR("send_buf full for fd=%d", sockfd);
                rte_pktmbuf_free(mbuf);
                return -1;
        }

        LOG_INFO(
            "nsendto fd=%d " IP_FMT ":%u -> " IP_FMT ":%u len=%zu data=%.*s",
            sockfd, IP_ARG(host->local_ip_),
            rte_be_to_cpu_16(host->local_port_), IP_ARG(daddr->sin_addr.s_addr),
            rte_be_to_cpu_16(daddr->sin_port), len, (int)len,
            (const char *)buf);
        return (ssize_t)len;
}

int nclose(int sockfd) {
        struct local_addr *host = get_local_addr_from_fd(sockfd);
        if (host == NULL) {
                LOG_ERROR("get_local_addr_from_fd() failed");
                return -1;
        }

        /*
         * Stop future lookups before releasing resources owned by this socket.
         * The current design expects socket lifecycle calls to be serialized
         * with packet processing by the application.
         */
        LL_REMOVE(host, g_local_addr);
        free_queued_packets(host->recv_buf_);
        free_queued_packets(host->send_buf_);
        pthread_cond_destroy(&host->cond_);
        pthread_mutex_destroy(&host->mutex_);
        rte_ring_free(host->recv_buf_);
        rte_ring_free(host->send_buf_);
        rte_free(host);
        return 0;
}