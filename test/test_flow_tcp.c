/* Exercise the real Flow/HTTP parser with deterministic transport boundaries. */
#include "../traffic-gen/core/conn_pool.h"
#include "../traffic-gen/core/flow_pool.h"
#include "../traffic-gen/core/scenario.h"
#include "../traffic-gen/proto/http/http_client.h"
#include <assert.h>
#include <errno.h>
#include <rte_eal.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <string.h>

static const char response[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: keep-alive\r\n\r\nok\n";
static const uint8_t request[] = "GET / HTTP/1.1\r\nHost: test\r\n\r\n";
static const char *input;
static int recv_error;
static bool eof, fail_after_partial_send;
static unsigned closed, completed, sends;
static enum tg_flow_result last_result;

ssize_t __wrap_owner_io_recv(struct nsock_handle h, void *buf, size_t len) {
        (void)h;
        if (input && *input) {
                size_t n = strlen(input);
                if (n > len) n = len;
                memcpy(buf, input, n);
                input += n;
                return (ssize_t)n;
        }
        if (eof) return 0;
        errno = recv_error ? recv_error : EAGAIN;
        return -1;
}
ssize_t __wrap_owner_io_send(struct nsock_handle h, const void *buf, size_t len) {
        (void)h;
        (void)buf;
        sends++;
        if (fail_after_partial_send) {
                if (sends == 1)
                        return 2;
                errno = EIO;
                return -1;
        }
        return (ssize_t)len;
}
int __wrap_owner_io_close(struct nsock_handle h) {
        (void)h;
        closed++;
        return 0;
}
static void finish(void *ctx, const struct tg_flow *flow,
                    enum tg_flow_result result) {
        (void)ctx;
        assert(flow->txn.proto != NULL);
        completed++;
        last_result = result;
}

struct fixture {
        struct tg_flow_map map;
        struct tg_flow_pool pool;
        struct tg_conn_pool connections;
        struct tg_class_plan cls;
        struct tg_http_config http;
        struct tg_flow *flow;
};
static void init(struct fixture *f) {
        memset(f, 0, sizeof(*f));
        input = NULL;
        recv_error = 0;
        eof = false;
        fail_after_partial_send = false;
        closed = completed = sends = 0;
        f->http = tg_http_bootstrap_config;
        f->http.connection_close = false;
        assert(tg_flow_map_init_with_capacity(&f->map, rte_lcore_id(), 1) == 0);
        assert(tg_flow_pool_init(&f->pool, 1) == 0);
        assert(tg_conn_pool_init(&f->connections, 1) == 0);
        f->flow = tg_flow_pool_get(&f->pool);
        assert(f->flow);
        struct nsock_handle h = {.id = 0, .owner_lcore = rte_lcore_id(),
                                  .generation = 1, .protocol = IPPROTO_TCP};
        assert(tg_flow_map_insert(&f->map, f->flow, h) == 0);
        assert(tg_conn_pool_attach(&f->connections, f->flow, &f->cls) == 0);
        assert(tg_txn_init_with_request(&f->flow->txn, &tg_http_proto_ops,
                                         &f->http, request, sizeof(request)-1) == 0);
        f->flow->state = TG_FLOW_RECEIVING;
        f->flow->requests_started = 1;
        f->flow->on_finish = finish;
}
static void fini(struct fixture *f) {
        if (f->flow->mapped)
                tg_flow_close_connection(&f->map, &f->pool, f->flow, false,
                                           TG_FLOW_RESULT_SUCCESS);
        assert(f->pool.free_count == 1 && f->connections.connections == 0);
        tg_conn_pool_fini(&f->connections);
        tg_flow_pool_fini(&f->pool);
        tg_flow_map_fini(&f->map);
}
static void receive(struct fixture *f, const char *bytes, bool end) {
        input = bytes;
        eof = end;
        tg_flow_on_event(&f->map, &f->pool, f->flow,
                           OWNER_IO_EV_READ | (end ? OWNER_IO_EV_HUP : 0));
}
int main(int argc, char **argv) {
        assert(rte_eal_init(argc, argv) >= 0);
        struct fixture f;
        init(&f);
        for (unsigned i = 0; i < 1000; i++) {
                receive(&f, response, false);
                assert(completed == i+1 && last_result == TG_FLOW_RESULT_SUCCESS);
                assert(closed == 0 && f.flow->state == TG_FLOW_IDLE);
                assert(tg_conn_pool_take_idle(&f.connections, &f.cls) == f.flow);
                assert(tg_flow_rearm_tcp(f.flow, &tg_http_proto_ops, &f.http,
                                           request, sizeof(request)-1) == 0);
        }
        fini(&f);

        /* FIN can be visible to recv while HUP is still queued behind the
         * reactor's event budget. A new request must not be sent on that fd.
         */
        init(&f);
        receive(&f, response, false);
        assert(tg_conn_pool_take_idle(&f.connections, &f.cls) == f.flow);
        eof = true;
        assert(tg_flow_rearm_tcp(f.flow, &tg_http_proto_ops, &f.http,
                                   request, sizeof(request)-1) == -1);
        assert(errno == ESTALE && sends == 0 && completed == 1);
        fini(&f);

        /* Once any bytes were accepted, a send error must not look like
         * the preflight ESTALE that permits replacement without replay. */
        init(&f);
        receive(&f, response, false);
        assert(tg_conn_pool_take_idle(&f.connections, &f.cls) == f.flow);
        fail_after_partial_send = true;
        assert(tg_flow_rearm_tcp(f.flow, &tg_http_proto_ops, &f.http,
                                   request, sizeof(request)-1) == -1);
        assert(errno == EIO && sends == 2 && f.flow->txn.request_offset == 2);
        assert(completed == 1);
        fini(&f);

        /* An explicit lifetime limit is honored, including the first request. */
        init(&f);
        f.connections.max_requests = 2;
        receive(&f, response, false);
        assert(closed == 0);
        assert(tg_conn_pool_take_idle(&f.connections, &f.cls) == f.flow);
        assert(tg_flow_rearm_tcp(f.flow, &tg_http_proto_ops, &f.http,
                                   request, sizeof(request)-1) == 0);
        receive(&f, response, false);
        assert(completed == 2 && closed == 1);
        fini(&f);

        init(&f);
        f.connections.max_requests = 1;
        receive(&f, response, false);
        assert(completed == 1 && closed == 1);
        fini(&f);

        init(&f);
        f.flow->requests_started = UINT64_MAX;
        receive(&f, response, false);
        assert(tg_conn_pool_take_idle(&f.connections, &f.cls) == f.flow);
        assert(tg_flow_rearm_tcp(f.flow, &tg_http_proto_ops, &f.http,
                                   request, sizeof(request)-1) == 0);
        assert(f.flow->requests_started == UINT64_MAX && closed == 0);
        fini(&f);

        /* A split response/FIN must not make the client actively close.
         * Completion is reported at the response, not delayed until FIN.
         */
        init(&f);
        f.http.connection_close = true;
        f.flow->deadline_cycles = rte_get_timer_cycles() + rte_get_timer_hz();
        receive(&f, response, false);
        assert(completed == 1 && last_result == TG_FLOW_RESULT_SUCCESS);
        assert(closed == 0 && f.flow->state == TG_FLOW_CLOSING);
        assert(tg_conn_pool_take_idle(&f.connections, &f.cls) == NULL);
        receive(&f, NULL, true);
        assert(closed == 1 && completed == 1);
        fini(&f);

        /* A keep-alive client also waits when the server retires a socket. */
        init(&f);
        receive(&f, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                    "Connection: close\r\n\r\n", false);
        assert(completed == 1 && f.flow->state == TG_FLOW_CLOSING);
        receive(&f, NULL, true);
        assert(closed == 1 && completed == 1);
        fini(&f);

        /* A peer that promises close but never sends FIN must not leak. */
        init(&f);
        f.http.connection_close = true;
        f.flow->deadline_cycles = rte_get_timer_cycles() + rte_get_timer_hz();
        receive(&f, response, false);
        tg_flow_expire(&f.map, &f.pool, f.flow->deadline_cycles);
        assert(closed == 1 && completed == 1);
        fini(&f);

        /* Coalesced complete response/FIN must not re-enter the idle pool. */
        init(&f);
        receive(&f, response, true);
        assert(completed == 1 && last_result == TG_FLOW_RESULT_SUCCESS);
        assert(closed == 1 && f.connections.connections == 0);
        fini(&f);

        /* An idle connection reset has no outstanding transaction to fail. */
        init(&f);
        receive(&f, response, false);
        tg_flow_on_event(&f.map, &f.pool, f.flow, OWNER_IO_EV_ERROR);
        assert(completed == 1 && closed == 1);
        fini(&f);

        init(&f);
        recv_error = ECONNRESET;
        receive(&f, NULL, false);
        assert(completed == 1 && last_result == TG_FLOW_RESULT_IO_FAILURE);
        fini(&f);

        /* Real malformed/truncated responses must still fail validation. */
        init(&f);
        receive(&f, "not HTTP\r\n", true);
        assert(completed == 1 && last_result == TG_FLOW_RESULT_PROTOCOL_FAILURE);
        fini(&f);
        init(&f);
        receive(&f, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nx", true);
        assert(completed == 1 && last_result == TG_FLOW_RESULT_PROTOCOL_FAILURE);
        fini(&f);
        assert(rte_eal_cleanup() == 0);
        return 0;
}
