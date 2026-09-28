/* Linked only into traffic-gen-resource-test. Exercise the production drain
 * and report path with a stalled TCP teardown or a retained pool object. */
#include "../pro-stack/owner_io.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/socket_owner_internal.h"
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>

int __real_owner_io_close(struct nsock_handle handle);
int __real_owner_timer_arm_after_ms(struct owner_timer *timer, uint64_t delay);

int __wrap_owner_timer_arm_after_ms(struct owner_timer *timer, uint64_t delay) {
        /* Only shorten the application's two-minute drain guard. */
        if (delay == 120000 && getenv("SNOWTG_RESOURCE_FAULT") != NULL)
                delay = 30;
        return __real_owner_timer_arm_after_ms(timer, delay);
}

int __wrap_owner_io_close(struct nsock_handle handle) {
        const char *fault = getenv("SNOWTG_RESOURCE_FAULT");
        struct nsock *sk = socket_owner_resolve_local(handle);
        static _Thread_local bool retained;
        if (fault != NULL && sk != NULL) {
                if (strcmp(fault, "forced") == 0 && sk->protocol == IPPROTO_TCP) {
                        /* Simulate a peer whose final teardown never completes. */
                        (void)owner_timer_cancel(&sk->u.tcp.timer);
                        tcp_stream_set_status(sk, TCP_STATUS_TIME_WAIT);
                        sk->app_closed = true;
                        return 0;
                }
                if (strcmp(fault, "residual") == 0 && !retained) {
                        retained = tcp_memory_tx_chunk_alloc(socket_owner_tcp_memory()) != NULL;
                }
        }
        return __real_owner_io_close(handle);
}
