#ifndef TCP_TESTING
#define TCP_TESTING
#endif

#include "../pro-stack/config.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/tcp_cc.h"
#include "../pro-stack/tcp_cc/newreno.h"
#include "../pro-stack/tcp_options.h"
#include "../pro-stack/tcp_rtt.h"

#include <rte_byteorder.h>
#include <rte_eal.h>
#include <rte_memcpy.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define CHECK(condition)                                                       \
        do {                                                                   \
                if (!(condition)) {                                            \
                        fprintf(stderr, "%s:%d: check failed: %s\n",          \
                                __FILE__, __LINE__, #condition);                \
                        exit(EXIT_FAILURE);                                    \
                }                                                              \
        } while (0)

struct wire_header {
        struct rte_tcp_hdr hdr;
        uint8_t options[40];
};

static void wire_init(struct wire_header *wire, uint8_t flags,
                      const uint8_t *options, size_t len) {
        memset(wire, 0, sizeof(*wire));
        CHECK(len <= sizeof(wire->options));
        CHECK((len & 3U) == 0);
        wire->hdr.tcp_flags = flags;
        wire->hdr.data_off = (uint8_t)((5 + len / 4) << 4);
        memcpy(wire->options, options, len);
}

static uint32_t read_be32(const uint8_t *p) {
        uint32_t value;

        rte_memcpy(&value, p, sizeof(value));
        return rte_be_to_cpu_32(value);
}

static void write_be32(uint8_t *p, uint32_t value) {
        value = rte_cpu_to_be_32(value);
        rte_memcpy(p, &value, sizeof(value));
}

static void test_option_parser(void) {
        struct wire_header wire;
        struct tcp_options_rx rx;
        const uint8_t permitted[4] = {4, 2, 0, 0};

        wire_init(&wire, RTE_TCP_SYN_FLAG, permitted, sizeof(permitted));
        CHECK(tcp_options_parse(&wire.hdr, &rx) == 0);
        CHECK(rx.sack_permitted_present);

        wire_init(&wire, RTE_TCP_ACK_FLAG, permitted, sizeof(permitted));
        CHECK(tcp_options_parse(&wire.hdr, &rx) == 0);
        CHECK(!rx.sack_permitted_present);

        const uint8_t duplicate[4] = {4, 2, 4, 2};
        wire_init(&wire, RTE_TCP_SYN_FLAG, duplicate, sizeof(duplicate));
        CHECK(tcp_options_parse(&wire.hdr, &rx) == -1);
        wire_init(&wire, RTE_TCP_ACK_FLAG, duplicate, sizeof(duplicate));
        CHECK(tcp_options_parse(&wire.hdr, &rx) == -1);

        uint8_t sack[12] = {5, 10};
        write_be32(sack + 2, UINT32_C(0xfffffff0));
        write_be32(sack + 6, UINT32_C(0x00000010));
        wire_init(&wire, RTE_TCP_ACK_FLAG, sack, sizeof(sack));
        CHECK(tcp_options_parse(&wire.hdr, &rx) == 0);
        CHECK(rx.sack_count == 1);
        CHECK(rx.sacks[0].left == UINT32_C(0xfffffff0));
        CHECK(rx.sacks[0].right == UINT32_C(0x00000010));

        sack[1] = 9;
        wire_init(&wire, RTE_TCP_ACK_FLAG, sack, sizeof(sack));
        CHECK(tcp_options_parse(&wire.hdr, &rx) == -1);
        sack[1] = 10;
        write_be32(sack + 6, UINT32_C(0xfffffff0));
        wire_init(&wire, RTE_TCP_ACK_FLAG, sack, sizeof(sack));
        CHECK(tcp_options_parse(&wire.hdr, &rx) == -1);

        uint8_t two_sacks[24] = {5, 10};
        write_be32(two_sacks + 2, 100);
        write_be32(two_sacks + 6, 110);
        two_sacks[10] = 5;
        two_sacks[11] = 10;
        write_be32(two_sacks + 12, 120);
        write_be32(two_sacks + 16, 130);
        wire_init(&wire, RTE_TCP_ACK_FLAG, two_sacks, sizeof(two_sacks));
        CHECK(tcp_options_parse(&wire.hdr, &rx) == -1);
}

static void test_negotiation_and_syn_emission(void) {
        struct nsock sk;
        struct tcp_fragment fragment;
        struct tcp_options_rx peer;

        memset(&sk, 0, sizeof(sk));
        memset(&fragment, 0, sizeof(fragment));
        memset(&peer, 0, sizeof(peer));
        peer.mss = TCP_DEFAULT_MSS;
        peer.sack_permitted_present = true;
        fragment.tcp_flags = RTE_TCP_SYN_FLAG;

        tcp_options_reset_state(&sk);
        CHECK(tcp_options_apply_syn(&sk, &fragment, true, true, true, 0) == 0);
        CHECK(fragment.opt_len == 6);
        CHECK(sk.u.tcp.sack_local_offered);
        CHECK(!sk.u.tcp.sack_permitted);
        CHECK(((const uint8_t *)fragment.options)[20] == 4);
        CHECK(((const uint8_t *)fragment.options)[21] == 2);
        tcp_options_negotiate_syn(&sk, &peer);
        CHECK(sk.u.tcp.sack_permitted);

        tcp_options_reset_state(&sk);
        tcp_options_negotiate_syn(&sk, &peer);
        CHECK(!sk.u.tcp.sack_permitted);
        memset(&fragment, 0, sizeof(fragment));
        fragment.tcp_flags = RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG;
        CHECK(tcp_options_apply_syn(&sk, &fragment, true, true, true, 123) ==
              0);
        CHECK(sk.u.tcp.sack_permitted);
}

static void link_ofo(struct nsock *sk, struct tcp_ofo_seg *segments,
                     const uint32_t *seqs, size_t count) {
        memset(segments, 0, count * sizeof(*segments));
        sk->u.tcp.ofo = count == 0 ? NULL : &segments[0];
        sk->u.tcp.ofo_tail = count == 0 ? NULL : &segments[count - 1];
        for (size_t i = 0; i < count; i++) {
                segments[i].seq = seqs[i];
                segments[i].len = 10;
                segments[i].prev = i == 0 ? NULL : &segments[i - 1];
                segments[i].next = i + 1 == count ? NULL : &segments[i + 1];
        }
}

static void test_receiver_sack_emission_and_mss(void) {
        struct nsock sk;
        struct tcp_fragment fragment;
        struct tcp_ofo_seg segments[4];
        const uint32_t adjacent[] = {1100, 1110, 1200};

        memset(&sk, 0, sizeof(sk));
        sk.u.tcp.sack_permitted = true;
        sk.u.tcp.recv_ack = 1000;
        sk.u.tcp.snd_mss = TCP_DEFAULT_MSS;
        link_ofo(&sk, segments, adjacent, ARRAY_SIZE(adjacent));
        sk.u.tcp.sack_recent_valid = true;
        sk.u.tcp.sack_recent.left = 1200;
        sk.u.tcp.sack_recent.right = 1210;

        memset(&fragment, 0, sizeof(fragment));
        fragment.tcp_flags = RTE_TCP_ACK_FLAG;
        CHECK(tcp_options_apply_established(&sk, &fragment) == 0);
        const uint8_t *opt = (const uint8_t *)fragment.options;
        CHECK(fragment.opt_len == 5);
        CHECK(opt[0] == 5 && opt[1] == 18);
        CHECK(read_be32(opt + 2) == 1200);
        CHECK(read_be32(opt + 6) == 1210);
        CHECK(read_be32(opt + 10) == 1100);
        CHECK(read_be32(opt + 14) == 1120);

        const uint32_t four[] = {1100, 1200, 1300, 1400};
        link_ofo(&sk, segments, four, ARRAY_SIZE(four));
        sk.u.tcp.sack_recent_valid = true;
        sk.u.tcp.sack_recent.left = 1400;
        sk.u.tcp.sack_recent.right = 1410;
        sk.u.tcp.timestamps_ok = false;
        memset(&fragment, 0, sizeof(fragment));
        fragment.tcp_flags = RTE_TCP_ACK_FLAG;
        CHECK(tcp_options_apply_established(&sk, &fragment) == 0);
        opt = (const uint8_t *)fragment.options;
        CHECK(fragment.opt_len == 9);
        CHECK(opt[0] == 5 && opt[1] == 34);
        CHECK(read_be32(opt + 2) == 1400);
        CHECK(tcp_options_data_mss(&sk, &fragment) ==
              TCP_DEFAULT_MSS - 36);

        sk.u.tcp.timestamps_ok = true;
        sk.u.tcp.ts_recent = 77;
        memset(&fragment, 0, sizeof(fragment));
        fragment.tcp_flags = RTE_TCP_ACK_FLAG;
        CHECK(tcp_options_apply_established(&sk, &fragment) == 0);
        opt = (const uint8_t *)fragment.options;
        CHECK(fragment.opt_len == TCP_MAX_OPTIONS);
        CHECK(opt[2] == 8);
        CHECK(opt[12] == 5 && opt[13] == 26);
        CHECK(read_be32(opt + 14) == 1400);
        CHECK(tcp_options_data_mss(&sk, &fragment) ==
              TCP_DEFAULT_MSS - 40);

        fragment.tcp_flags = 0;
        CHECK(tcp_options_apply_established(&sk, &fragment) == 0);
        CHECK(fragment.opt_len == 3);
}

static void init_score_socket(struct nsock *sk, uint32_t una,
                              uint32_t flight_len) {
        memset(sk, 0, sizeof(*sk));
        sk->u.tcp.sack_permitted = true;
        sk->u.tcp.snd_mss = 100;
        sk->u.tcp.snd_una = una;
        sk->u.tcp.sent_seq = una + flight_len;
        sk->u.tcp.sndbuf.head_seq = una;
        sk->u.tcp.sndbuf.len = flight_len;
        tcp_sack_state_init(&sk->u.tcp, una);
        tcp_sack_note_new_data(&sk->u.tcp, una + flight_len);
        tcp_cc_set_ops(&sk->u.tcp, &tcp_newreno_ops, false);
}

static const struct tcp_sack_range *score_at(const struct nsock *sk,
                                             uint16_t index) {
        const struct tcp_sack_range *range = sk->u.tcp.sack.sacked;

        while (range != NULL && index-- != 0)
                range = range->next;
        return range;
}

static void test_sender_scoreboard(void) {
        struct nsock sk;
        const struct tcp_sack_block overlap[] = {
            {1500, 1700}, {1600, 1800}, {1200, 1300}};

        init_score_socket(&sk, 1000, 1000);
        tcp_test_sack_score_update(&sk, overlap, ARRAY_SIZE(overlap));
        CHECK(tcp_sack_score_count(&sk.u.tcp) == 2);
        CHECK(score_at(&sk, 0)->left == 1200);
        CHECK(score_at(&sk, 0)->right == 1300);
        CHECK(score_at(&sk, 1)->left == 1500);
        CHECK(score_at(&sk, 1)->right == 1800);

        uint32_t snd_nxt = sk.u.tcp.sent_seq;
        CHECK(tcp_test_sack_schedule_retransmit(&sk));
        CHECK(sk.u.tcp.sack.pending.seq == 1000);
        CHECK(sk.u.tcp.sack.pending.end == 1100);
        CHECK(sk.u.tcp.sent_seq == snd_nxt);

        sk.u.tcp.snd_una = 1250;
        sk.u.tcp.sndbuf.head_seq = 1250;
        sk.u.tcp.sndbuf.len = 750;
        tcp_test_sack_score_update(&sk, NULL, 0);
        CHECK(tcp_sack_score_count(&sk.u.tcp) == 2);
        CHECK(score_at(&sk, 0)->left == 1250);
        CHECK(score_at(&sk, 0)->right == 1300);
        tcp_test_sack_score_update(&sk, NULL, 0);
        tcp_test_sack_score_update(&sk, NULL, 0);
        CHECK(tcp_sack_score_count(&sk.u.tcp) == 2);

        tcp_test_sack_score_clear(&sk);
        init_score_socket(&sk, 1000, 1000);
        const struct tcp_sack_block clipped[] = {
            {900, 1100}, {1900, 2100}};
        tcp_test_sack_score_update(&sk, clipped, ARRAY_SIZE(clipped));
        CHECK(tcp_sack_score_count(&sk.u.tcp) == 2);
        CHECK(score_at(&sk, 0)->left == 1000);
        CHECK(score_at(&sk, 1)->right == 2000);

        tcp_test_sack_score_clear(&sk);
        init_score_socket(&sk, 1000, 1000);
        const struct tcp_sack_block all = {1000, 2000};
        tcp_test_sack_score_update(&sk, &all, 1);
        CHECK(tcp_sack_score_count(&sk.u.tcp) == 1);

        tcp_test_sack_score_clear(&sk);
        init_score_socket(&sk, UINT32_C(0xffffff00), 512);
        const struct tcp_sack_block wrapped = {0, 128};
        tcp_test_sack_score_update(&sk, &wrapped, 1);
        CHECK(tcp_test_sack_schedule_retransmit(&sk));
        CHECK(sk.u.tcp.sack.pending.seq == UINT32_C(0xffffff00));
        CHECK(sk.u.tcp.sack.pending.end == UINT32_C(0xffffff64));
        tcp_test_sack_score_clear(&sk);
}

static void test_scoreboard_capacity_fallback(void) {
        struct nsock sk;

        init_score_socket(&sk, 1000, 10000);
        for (uint32_t i = 0; i < TCP_SACK_SCORE_MAX_RANGES; i += 4) {
                struct tcp_sack_block blocks[4];
                for (uint32_t j = 0; j < ARRAY_SIZE(blocks); j++) {
                        blocks[j].left = 1010 + (i + j) * 10;
                        blocks[j].right = blocks[j].left + 1;
                }
                tcp_test_sack_score_update(&sk, blocks, ARRAY_SIZE(blocks));
        }
        CHECK(tcp_sack_score_count(&sk.u.tcp) ==
              TCP_SACK_SCORE_MAX_RANGES);
        const struct tcp_sack_block overflow = {5000, 5001};
        tcp_test_sack_score_update(&sk, &overflow, 1);
        CHECK(tcp_sack_score_count(&sk.u.tcp) == 0);
        CHECK(sk.u.tcp.sack.degraded);
        tcp_test_sack_score_clear(&sk);
}

static void test_dsack_and_recovery_algorithms(void) {
        struct nsock sk;

        init_score_socket(&sk, 1000, 1000);
        const struct tcp_sack_block dsacks[] = {
            {900, 950}, {1300, 1400}};
        struct tcp_sack_ack_result result = tcp_sack_update(
            &sk.u.tcp, 1000, dsacks, ARRAY_SIZE(dsacks), 2000);
        CHECK(result.dsack_valid);
        CHECK(result.dsack.left == 900 && result.dsack.right == 950);
        CHECK(result.newly_sacked_bytes == 100);

        const struct tcp_sack_block loss_blocks[] = {
            {1200, 1300}, {1400, 1500}, {1600, 1700}};
        result = tcp_sack_update(&sk.u.tcp, 1000, loss_blocks,
                                 ARRAY_SIZE(loss_blocks), 2000);
        CHECK(result.new_sack_information);
        CHECK(tcp_sack_is_lost(&sk.u.tcp, 1000));
        CHECK(tcp_sack_set_pipe(&sk.u.tcp, 2000) < 1000);

        tcp_sack_enter_recovery(&sk.u.tcp, TCP_RECOVERY_SACK, 2000);
        CHECK(sk.u.tcp.sack.pending.kind == TCP_RECOVERY_TX_RETRANSMIT);
        CHECK(sk.u.tcp.sack.pending.seq == 1000);
        tcp_sack_commit_candidate(&sk.u.tcp, 1100);
        CHECK(sk.u.tcp.sack.high_rxt == 1100);
        const struct tcp_sack_block retrans_dsack = {1000, 1100};
        result = tcp_sack_update(&sk.u.tcp, 1100, &retrans_dsack, 1, 2000);
        CHECK(result.dsack_valid);
        CHECK(result.dsack_covers_retransmission);
        CHECK(tcp_sack_schedule_next(&sk.u.tcp, 2200, true));
        CHECK(sk.u.tcp.sack.pending.nextseg_rule >= 1 &&
              sk.u.tcp.sack.pending.nextseg_rule <= 4);

        uint32_t snd_nxt = sk.u.tcp.sent_seq;
        tcp_sack_on_rto(&sk.u.tcp, 2000);
        CHECK(sk.u.tcp.sack.mode == TCP_RECOVERY_RTO);
        CHECK(tcp_sack_score_count(&sk.u.tcp) == 0);
        CHECK(sk.u.tcp.sack.pending.seq == sk.u.tcp.snd_una);
        CHECK(sk.u.tcp.sent_seq == snd_nxt);
        tcp_test_sack_score_clear(&sk);
}

static void test_newreno_vtable(void) {
        struct nsock sk;
        struct tcp_cc_ack_event ack;
        struct tcp_cc_loss_event loss;

        memset(&sk, 0, sizeof(sk));
        sk.u.tcp.snd_mss = 1460;
        tcp_cc_set_ops(&sk.u.tcp, &tcp_newreno_ops, false);
        CHECK(sk.u.tcp.cc.cwnd == 3U * 1460U);

        memset(&ack, 0, sizeof(ack));
        ack.acked_bytes = 1460;
        ack.cwnd_limited = true;
        tcp_cc_on_ack(&sk.u.tcp, &ack);
        CHECK(sk.u.tcp.cc.cwnd == 4U * 1460U);

        memset(&loss, 0, sizeof(loss));
        loss.reason = TCP_CC_LOSS_SACK;
        loss.flight_size = 8U * 1460U;
        tcp_cc_on_loss(&sk.u.tcp, &loss);
        CHECK(sk.u.tcp.cc.ssthresh == 4U * 1460U);
        CHECK(sk.u.tcp.cc.cwnd == sk.u.tcp.cc.ssthresh);

        loss.reason = TCP_CC_LOSS_RTO;
        loss.first_rto_for_seq = true;
        tcp_cc_on_rto(&sk.u.tcp, &loss);
        CHECK(sk.u.tcp.cc.cwnd == 1460U);
}

static void test_raw_timestamp_rtt_sample(void) {
        struct nsock sk;
        uint32_t sample = 0;

        memset(&sk, 0, sizeof(sk));
        sk.u.tcp.timestamps_ok = true;
        CHECK(tcp_rtt_sample_ack(&sk, true, UINT32_MAX - 4U, 5U, &sample));
        CHECK(sample == 10U);

        sk.u.tcp.rtt_retransmitting = true;
        CHECK(!tcp_rtt_sample_ack(&sk, true, 1U, 2U, &sample));
}

static void test_duplicate_ack_recovery_entry(void) {
        struct nsock sk;
        const struct tcp_sack_block blocks[] = {
            {1200, 1300}, {1400, 1500}, {1600, 1700}};

        init_score_socket(&sk, 1000, 1000);
        CHECK(pthread_mutex_init(&sk.mutex, NULL) == 0);
        for (size_t i = 0; i < ARRAY_SIZE(blocks); i++) {
                sk.u.tcp.rx_sack_count = 1;
                sk.u.tcp.rx_sacks[0] = blocks[i];
                tcp_test_process_peer_ack(&sk, 1000, false);
        }
        CHECK(sk.u.tcp.sack.mode == TCP_RECOVERY_SACK);
        CHECK(sk.u.tcp.sack.recovery_point == 2000);
        CHECK(sk.u.tcp.sack.pending.kind == TCP_RECOVERY_TX_RETRANSMIT);
        CHECK(sk.u.tcp.sack.pending.seq == 1000);
        CHECK(sk.u.tcp.cc.cwnd == sk.u.tcp.cc.ssthresh);
        uint32_t sack_recovery_cwnd = sk.u.tcp.cc.cwnd;
        sk.u.tcp.rx_sack_count = 1;
        sk.u.tcp.rx_sacks[0] = (struct tcp_sack_block){1800, 1900};
        tcp_test_process_peer_ack(&sk, 1000, false);
        CHECK(sk.u.tcp.cc.cwnd == sack_recovery_cwnd);
        tcp_test_sack_score_clear(&sk);
        pthread_mutex_destroy(&sk.mutex);

        init_score_socket(&sk, 1000, 1000);
        sk.u.tcp.sack_permitted = false;
        CHECK(pthread_mutex_init(&sk.mutex, NULL) == 0);
        for (unsigned int i = 0; i < TCP_SACK_DUP_THRESH; i++)
                tcp_test_process_peer_ack(&sk, 1000, true);
        CHECK(sk.u.tcp.sack.mode == TCP_RECOVERY_NEWRENO);
        CHECK(sk.u.tcp.sack.pending.seq == 1000);

        uint32_t recovery_cwnd = sk.u.tcp.cc.cwnd;
        tcp_test_process_peer_ack(&sk, 1100, false);
        CHECK(sk.u.tcp.sack.mode == TCP_RECOVERY_NEWRENO);
        CHECK(sk.u.tcp.sack.pending.seq == 1100);
        CHECK(sk.u.tcp.sack.pending.end == 1200);
        CHECK(sk.u.tcp.cc.cwnd <= recovery_cwnd);

        tcp_test_process_peer_ack(&sk, 2000, false);
        CHECK(sk.u.tcp.sack.mode == TCP_RECOVERY_NORMAL);
        CHECK(sk.u.tcp.sack.pending.kind == TCP_RECOVERY_TX_NONE);
        CHECK(sk.u.tcp.cc.cwnd == sk.u.tcp.cc.ssthresh);
        tcp_test_sack_score_clear(&sk);
        pthread_mutex_destroy(&sk.mutex);
}

static void test_dsack_emission(void) {
        struct nsock sk;
        struct tcp_fragment fragment;

        memset(&sk, 0, sizeof(sk));
        sk.u.tcp.sack_permitted = true;
        sk.u.tcp.recv_ack = 2000;
        sk.u.tcp.snd_mss = TCP_DEFAULT_MSS;
        sk.u.tcp.dsack_pending = true;
        sk.u.tcp.dsack_block.left = 1500;
        sk.u.tcp.dsack_block.right = 1600;
        memset(&fragment, 0, sizeof(fragment));
        fragment.tcp_flags = RTE_TCP_ACK_FLAG;
        CHECK(tcp_options_apply_established(&sk, &fragment) == 0);
        const uint8_t *opt = (const uint8_t *)fragment.options;
        CHECK(opt[0] == 5 && opt[1] == 10);
        CHECK(read_be32(opt + 2) == 1500);
        CHECK(read_be32(opt + 6) == 1600);
        CHECK(!sk.u.tcp.dsack_pending);
}

static void test_ofo_duplicate_dsack(void) {
        struct nsock sk;
        struct tcp_fragment fragment;
        const uint8_t data[16] = {0};

        memset(&sk, 0, sizeof(sk));
        tcp_test_ofo_init(&sk, 100, 4096);
        sk.u.tcp.sack_permitted = true;
        sk.u.tcp.snd_mss = TCP_DEFAULT_MSS;
        CHECK(tcp_test_ofo_insert(&sk, 120, data, 10, 0) == 0);
        CHECK(tcp_test_ofo_insert(&sk, 122, data, 4, 0) == 0);
        CHECK(sk.u.tcp.dsack_pending);
        CHECK(sk.u.tcp.dsack_block.left == 122);
        CHECK(sk.u.tcp.dsack_block.right == 126);

        memset(&fragment, 0, sizeof(fragment));
        fragment.tcp_flags = RTE_TCP_ACK_FLAG;
        CHECK(tcp_options_apply_established(&sk, &fragment) == 0);
        const uint8_t *opt = (const uint8_t *)fragment.options;
        CHECK(opt[0] == 5 && opt[1] == 18);
        CHECK(read_be32(opt + 2) == 122);
        CHECK(read_be32(opt + 6) == 126);
        CHECK(read_be32(opt + 10) == 120);
        CHECK(read_be32(opt + 14) == 130);

        CHECK(tcp_test_ofo_insert(&sk, 90, data, 5, 0) == 0);
        CHECK(sk.u.tcp.dsack_pending);
        CHECK(sk.u.tcp.dsack_block.left == 90);
        CHECK(sk.u.tcp.dsack_block.right == 95);
        tcp_test_ofo_purge(&sk);
}

int main(void) {
        char *eal_argv[] = {"test_tcp_sack", "--in-memory", "--no-pci"};

        CHECK(rte_eal_init((int)ARRAY_SIZE(eal_argv), eal_argv) >= 0);
        test_option_parser();
        test_negotiation_and_syn_emission();
        test_receiver_sack_emission_and_mss();
        test_sender_scoreboard();
        test_scoreboard_capacity_fallback();
        test_dsack_and_recovery_algorithms();
        test_newreno_vtable();
        test_raw_timestamp_rtt_sample();
        test_duplicate_ack_recovery_entry();
        test_dsack_emission();
        test_ofo_duplicate_dsack();
        puts("test_tcp_sack: PASS");
        return EXIT_SUCCESS;
}
