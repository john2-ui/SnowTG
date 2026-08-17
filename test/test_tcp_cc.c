#include "../pro-stack/config.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/tcp_cc.h"
#include "../pro-stack/tcp_cc/cubic.h"
#include "../pro-stack/tcp_cc/newreno.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
        do {                                                                   \
                if (!(condition)) {                                            \
                        fprintf(stderr, "%s:%d: check failed: %s\n",          \
                                __FILE__, __LINE__, #condition);                \
                        exit(EXIT_FAILURE);                                    \
                }                                                              \
        } while (0)

static void init_stream(struct nsock *sk, const struct tcp_cc_ops *ops) {
        memset(sk, 0, sizeof(*sk));
        sk->u.tcp.snd_mss = 1460;
        tcp_cc_set_ops(&sk->u.tcp, ops, false);
}

static struct tcp_cc_ack_event ack_event(uint32_t acked) {
        struct tcp_cc_ack_event event;

        memset(&event, 0, sizeof(event));
        event.acked_bytes = acked;
        event.cwnd_limited = true;
        event.ack_seq = acked;
        event.snd_nxt = 64U * 1460U;
        event.now_ms = 1;
        return event;
}

static void test_default_selection(void) {
        struct nsock sk;

        memset(&sk, 0, sizeof(sk));
        sk.u.tcp.snd_mss = 1460;
        tcp_cc_init_default(&sk.u.tcp, false);
#if TCP_CC_DEFAULT_ALGO == TCP_CC_ALGO_CUBIC
        CHECK(strcmp(sk.u.tcp.cc.ops->name, "cubic") == 0);
#else
        CHECK(strcmp(sk.u.tcp.cc.ops->name, "newreno") == 0);
#endif
        CHECK(sk.u.tcp.cc.cwnd == 3U * 1460U);

        tcp_cc_init_default(&sk.u.tcp, true);
        CHECK(sk.u.tcp.cc.cwnd == 1460U);
}

static void test_newreno_growth_and_recovery(void) {
        struct nsock sk;
        struct tcp_cc_ack_event ack;
        struct tcp_cc_loss_event loss;

        init_stream(&sk, &tcp_newreno_ops);
        ack = ack_event(1460);
        tcp_cc_on_ack(&sk.u.tcp, &ack);
        CHECK(sk.u.tcp.cc.cwnd == 4U * 1460U);

        sk.u.tcp.cc.ssthresh = sk.u.tcp.cc.cwnd;
        for (unsigned int i = 0; i < 4; i++)
                tcp_cc_on_ack(&sk.u.tcp, &ack);
        CHECK(sk.u.tcp.cc.cwnd == 5U * 1460U);

        memset(&loss, 0, sizeof(loss));
        loss.reason = TCP_CC_LOSS_DUPACK;
        loss.flight_size = 8U * 1460U;
        tcp_cc_on_loss(&sk.u.tcp, &loss);
        CHECK(sk.u.tcp.cc.ssthresh == 4U * 1460U);
        CHECK(sk.u.tcp.cc.cwnd == 7U * 1460U);

        ack = ack_event(1460);
        ack.in_recovery = true;
        ack.newreno_recovery = true;
        ack.partial_ack = true;
        tcp_cc_on_ack(&sk.u.tcp, &ack);
        CHECK(sk.u.tcp.cc.cwnd == 7U * 1460U);

        ack.acked_bytes = 0;
        ack.partial_ack = false;
        ack.duplicate_ack = true;
        tcp_cc_on_ack(&sk.u.tcp, &ack);
        CHECK(sk.u.tcp.cc.cwnd == 8U * 1460U);
        tcp_cc_on_recovery_exit(&sk.u.tcp);
        CHECK(sk.u.tcp.cc.cwnd == 4U * 1460U);

        loss.reason = TCP_CC_LOSS_RTO;
        loss.first_rto_for_seq = true;
        tcp_cc_on_rto(&sk.u.tcp, &loss);
        CHECK(sk.u.tcp.cc.cwnd == 1460U);
}

static void test_cubic_slow_start_and_loss(void) {
        struct nsock sk;
        struct tcp_cc_ack_event ack;
        struct tcp_cc_loss_event loss;

        init_stream(&sk, &tcp_cubic_ops);
        ack = ack_event(1460);
        ack.cwnd_limited = false;
        tcp_cc_on_ack(&sk.u.tcp, &ack);
        CHECK(sk.u.tcp.cc.cwnd == 3U * 1460U);

        ack.cwnd_limited = true;
        tcp_cc_on_ack(&sk.u.tcp, &ack);
        CHECK(sk.u.tcp.cc.cwnd == 4U * 1460U);

        memset(&loss, 0, sizeof(loss));
        loss.reason = TCP_CC_LOSS_SACK;
        loss.flight_size = 10U * 1460U;
        tcp_cc_on_loss(&sk.u.tcp, &loss);
        CHECK(sk.u.tcp.cc.ssthresh == 7U * 1460U);
        CHECK(sk.u.tcp.cc.cwnd == 7U * 1460U);

        loss.reason = TCP_CC_LOSS_DUPACK;
        loss.flight_size = 20U * 1460U;
        tcp_cc_on_loss(&sk.u.tcp, &loss);
        CHECK(sk.u.tcp.cc.ssthresh == 14U * 1460U);
        CHECK(sk.u.tcp.cc.cwnd == 17U * 1460U);
        tcp_cc_on_recovery_exit(&sk.u.tcp);
        CHECK(sk.u.tcp.cc.cwnd == 14U * 1460U);

        loss.reason = TCP_CC_LOSS_RTO;
        loss.first_rto_for_seq = true;
        tcp_cc_on_rto(&sk.u.tcp, &loss);
        CHECK(sk.u.tcp.cc.cwnd == 1460U);
        CHECK(sk.u.tcp.cc.ssthresh == 14U * 1460U);
}

static void test_cubic_fixed_point_vectors(void) {
        uint32_t k_ms = tcp_cubic_test_k_ms(20000, 15000, 1000);

        CHECK(k_ms == 2320);
        CHECK(tcp_cubic_test_window_at(20000, k_ms, 1000, k_ms) ==
              20000);
        CHECK(tcp_cubic_test_window_at(20000, k_ms, 1000, k_ms - 1000) ==
              19600);
        CHECK(tcp_cubic_test_window_at(20000, k_ms, 1000, k_ms + 1000) ==
              20400);

        struct nsock sk;
        struct tcp_cc_loss_event loss;

        init_stream(&sk, &tcp_cubic_ops);
        memset(&loss, 0, sizeof(loss));
        loss.reason = TCP_CC_LOSS_SACK;
        loss.flight_size = sk.u.tcp.cc.cwnd;
        tcp_cc_on_loss(&sk.u.tcp, &loss);
        uint32_t previous_max = tcp_cubic_test_w_max(&sk.u.tcp);

        sk.u.tcp.cc.cwnd = previous_max - 100;
        loss.flight_size = sk.u.tcp.cc.cwnd;
        tcp_cc_on_loss(&sk.u.tcp, &loss);
        CHECK(tcp_cubic_test_w_max(&sk.u.tcp) < previous_max);
}

static void test_hystart_css_and_time_pause(void) {
        struct nsock sk;
        struct tcp_cc_ack_event ack = ack_event(1460);

        init_stream(&sk, &tcp_cubic_ops);
        ack.rtt_sample_valid = true;
        ack.rtt_sample_ms = 10;
        ack.snd_nxt = 1000;
        for (uint32_t i = 1; i <= 8; i++) {
                ack.ack_seq = i * 100;
                ack.now_ms++;
                tcp_cc_on_ack(&sk.u.tcp, &ack);
        }

        ack.rtt_sample_ms = 20;
        ack.snd_nxt = 2000;
        ack.ack_seq = 1000;
        tcp_cc_on_ack(&sk.u.tcp, &ack);
        for (uint32_t i = 1; i < 8; i++) {
                ack.ack_seq = 1000 + i * 100;
                ack.now_ms++;
                uint32_t before = sk.u.tcp.cc.cwnd;

                tcp_cc_on_ack(&sk.u.tcp, &ack);
                if (i == 7)
                        CHECK(sk.u.tcp.cc.cwnd - before == 1460U / 4U);
        }

        /* Force congestion avoidance, then verify an application-limited ACK
         * neither grows cwnd nor lets a long wall-clock gap advance the epoch. */
        sk.u.tcp.cc.ssthresh = sk.u.tcp.cc.cwnd;
        ack.rtt_sample_valid = false;
        ack.acked_bytes = 1460;
        ack.cwnd_limited = true;
        ack.now_ms = UINT32_MAX - 10U;
        tcp_cc_on_ack(&sk.u.tcp, &ack);
        uint32_t before = sk.u.tcp.cc.cwnd;

        ack.cwnd_limited = false;
        ack.now_ms = 20;
        tcp_cc_on_ack(&sk.u.tcp, &ack);
        CHECK(sk.u.tcp.cc.cwnd == before);

        ack.cwnd_limited = true;
        ack.now_ms = 21;
        tcp_cc_on_ack(&sk.u.tcp, &ack);
        CHECK(sk.u.tcp.cc.cwnd >= before);
        CHECK(sk.u.tcp.cc.cwnd <= before + before / 2U);
}

int main(void) {
        test_default_selection();
        test_newreno_growth_and_recovery();
        test_cubic_slow_start_and_loss();
        test_cubic_fixed_point_vectors();
        test_hystart_css_and_time_pause();
        puts("test_tcp_cc: PASS");
        return EXIT_SUCCESS;
}
