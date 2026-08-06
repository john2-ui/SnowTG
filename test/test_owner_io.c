#include "../pro-stack/owner_io.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/socket_owner_internal.h"

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <rte_byteorder.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_timer.h>

static void count_release(void *ctx) {
        unsigned int *count = ctx;

        (*count)++;
}

int main(int argc, char **argv) {
        assert(rte_eal_init(argc, argv) >= 0);
        rte_timer_subsystem_init();
        assert(socket_registry_init() == 0);
        assert(socket_owner_init(rte_lcore_id()) == 0);

        struct nsock_handle handle;
        assert(owner_io_socket_create(IPPROTO_UDP, &handle) == 0);
        struct nsock *sk = socket_owner_resolve_local(handle);
        assert(sk != NULL);

        char buf[8];
        errno = 0;
        assert(owner_io_recvfrom(handle, buf, sizeof(buf), NULL, NULL) == -1);
        assert(errno == EAGAIN);
        assert(sk->recv_wait_head == NULL);

        struct sockaddr addr_buf;
        socklen_t short_addr_len = sizeof(sa_family_t);
        errno = 0;
        assert(owner_io_recvfrom(handle, buf, sizeof(buf), &addr_buf,
                                 &short_addr_len) == -1);
        assert(errno == EINVAL);

        struct sockaddr_in invalid_peer = {
            .sin_family = AF_UNSPEC,
        };
        errno = 0;
        assert(owner_io_sendto(handle, buf, sizeof(buf),
                               (struct sockaddr *)&invalid_peer,
                               sizeof(invalid_peer)) == -1);
        assert(errno == EINVAL);
        errno = 0;
        assert(owner_io_sendto(handle, buf, sizeof(buf),
                               (struct sockaddr *)&invalid_peer,
                               sizeof(sa_family_t)) == -1);
        assert(errno == EINVAL);

        socket_owner_ready_post(sk, OWNER_IO_EV_READ);
        socket_owner_ready_post(sk, OWNER_IO_EV_WRITE);
        struct owner_io_event event[2];
        assert(owner_io_ready_burst(event, 2) == 1);
        assert(event[0].handle.id == handle.id);
        assert(event[0].handle.generation == handle.generation);
        assert(event[0].events == (OWNER_IO_EV_READ | OWNER_IO_EV_WRITE));

        socket_owner_ready_post(sk, OWNER_IO_EV_READ);
        assert(owner_io_close(handle) == 0);
        assert(owner_io_ready_burst(event, 2) == 0);

        struct nsock_handle wrong_owner = handle;
        wrong_owner.owner_lcore++;
        errno = 0;
        assert(owner_io_recvfrom(wrong_owner, buf, sizeof(buf), NULL, NULL) ==
               -1);
        assert(errno == EPERM);

        struct nsock_handle tcp_handle;
        assert(owner_io_socket_create(IPPROTO_TCP, &tcp_handle) == 0);
        struct sockaddr_in peer = {
            .sin_family = AF_INET,
            .sin_port = rte_cpu_to_be_16(80),
            .sin_addr.s_addr = rte_cpu_to_be_32(0x7f000001),
        };
        errno = 0;
        assert(owner_io_connect(tcp_handle, (struct sockaddr *)&peer,
                                sizeof(sa_family_t)) == -1);
        assert(errno == EINVAL);
        errno = 0;
        assert(owner_io_connect(tcp_handle, (struct sockaddr *)&peer,
                                sizeof(peer)) == -1);
        assert(errno == EINPROGRESS);
        assert(owner_io_close(tcp_handle) == 0);

        unsigned int release_count = 0;
        struct nsock_handle local_tcp;
        assert(owner_io_socket_create_local(IPPROTO_TCP, &local_tcp) == 0);
        sk = socket_owner_resolve_local(local_tcp);
        assert(sk != NULL);
        assert(sk->io_mode == NSOCK_IO_OWNER_LOCAL);
        assert(sk->recv_buf == NULL);
        assert(sk->send_buf == NULL);
        assert(owner_io_set_release_observer(local_tcp, count_release,
                                             &release_count) == 0);
        assert(owner_io_close(local_tcp) == 0);
        assert(release_count == 1);

        socket_registry_fini();
        return 0;
}
