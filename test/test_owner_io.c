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
                                sizeof(peer)) == -1);
        assert(errno == EINPROGRESS);
        assert(owner_io_close(tcp_handle) == 0);

        socket_registry_fini();
        return 0;
}
