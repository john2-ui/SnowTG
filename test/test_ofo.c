#ifndef TCP_TESTING
#define TCP_TESTING
#endif

#include "../pro-stack/config.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/socket_owner_internal.h"
#include "../pro-stack/tcp.h"

#include <limits.h>
#include <rte_eal.h>
#include <rte_malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* TCP window updates notify the owner in production; this unit test has none.
 */
void socket_owner_ready_post(struct nsock *sk, uint32_t events) {
        (void)sk;
        (void)events;
}

int nsock_tcp_rx_enqueue(struct nsock *sk, struct tcp_rx_blob *blob) {
        if (sk == NULL || blob == NULL || sk->u.tcp.rx_queue_count >= RING_SIZE)
                return -1;
        blob->next = NULL;
        if (sk->u.tcp.rx_queue_tail != NULL)
                sk->u.tcp.rx_queue_tail->next = blob;
        else
                sk->u.tcp.rx_queue_head = blob;
        sk->u.tcp.rx_queue_tail = blob;
        sk->u.tcp.rx_queue_count++;
        return 0;
}

#define CHECK(condition)                                                       \
        do {                                                                   \
                if (!(condition)) {                                            \
                        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
                                __LINE__, #condition);                         \
                        exit(EXIT_FAILURE);                                    \
                }                                                              \
        } while (0)

static int validate_rb_subtree(const struct rb_node *node,
                               const struct rb_node *parent) {
        int left_height;
        int right_height;

        if (node == NULL)
                return 1;
        CHECK(node->parent == parent);
        CHECK(node->color == RB_RED || node->color == RB_BLACK);
        if (node->color == RB_RED) {
                CHECK(node->left == NULL || node->left->color == RB_BLACK);
                CHECK(node->right == NULL || node->right->color == RB_BLACK);
        }
        left_height = validate_rb_subtree(node->left, node);
        right_height = validate_rb_subtree(node->right, node);
        CHECK(left_height == right_height);
        return left_height + (node->color == RB_BLACK ? 1 : 0);
}

static bool seq_before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

static void validate_indexes(const struct nsock *sk) {
        const struct tcp_ofo_seg *list_node;
        struct rb_node *tree_node;
        size_t list_count = 0;
        uint32_t bytes = 0;

        if (sk->u.tcp.ofo_tree.node != NULL)
                CHECK(sk->u.tcp.ofo_tree.node->color == RB_BLACK);
        (void)validate_rb_subtree(sk->u.tcp.ofo_tree.node, NULL);

        list_node = sk->u.tcp.ofo;
        tree_node = rb_first(&sk->u.tcp.ofo_tree);
        while (list_node != NULL) {
                CHECK(tree_node != NULL);
                CHECK(rb_entry(tree_node, struct tcp_ofo_seg, rb) == list_node);
                CHECK(list_node->prev ==
                      (list_count == 0 ? NULL
                                       : rb_entry(rb_prev(tree_node),
                                                  struct tcp_ofo_seg, rb)));
                if (list_node->next != NULL)
                        CHECK(seq_before(list_node->seq, list_node->next->seq));
                bytes += list_node->len;
                list_count++;
                list_node = list_node->next;
                tree_node = rb_next(tree_node);
        }
        CHECK(tree_node == NULL);
        CHECK(list_count == sk->u.tcp.ofo_count);
        CHECK(bytes == sk->u.tcp.ofo_bytes);
        CHECK(sk->u.tcp.ofo_tail ==
              (sk->u.tcp.ofo == NULL ? NULL
                                     : rb_entry(rb_last(&sk->u.tcp.ofo_tree),
                                                struct tcp_ofo_seg, rb)));
}

static void expect_segment(const struct tcp_ofo_seg *segment, uint32_t seq,
                           const char *data, uint32_t len, int has_fin) {
        CHECK(segment != NULL);
        CHECK(segment->seq == seq);
        CHECK(segment->len == len);
        CHECK(segment->has_fin == (uint8_t)has_fin);
        if (len != 0)
                CHECK(memcmp(segment->data, data, len) == 0);
}

static void init_socket(struct nsock *sk, uint32_t rcv_nxt, uint32_t rcvbuf) {
        memset(sk, 0, sizeof(*sk));
        tcp_test_ofo_init(sk, rcv_nxt, rcvbuf);
}

static void free_test_rx_queue(struct nsock *sk) {
        struct tcp_rx_blob *blob = sk->u.tcp.rx_queue_head;

        while (blob != NULL) {
                struct tcp_rx_blob *next = blob->next;

                rte_free(blob->data);
                rte_free(blob);
                blob = next;
        }
        sk->u.tcp.rx_queue_head = NULL;
        sk->u.tcp.rx_queue_tail = NULL;
        sk->u.tcp.rx_queue_count = 0;
}

static void test_ofo_metrics_lifecycle(void) {
        struct nsock sk;
        struct tcp_ofo_metrics metrics;
        static const char payload[] = "abcdefghij";

        tcp_test_ofo_metrics_reset();
        tcp_test_ofo_force_pressure(false);
        init_socket(&sk, 90, TCP_RCVBUF_SIZE);
        CHECK(tcp_test_ofo_insert(&sk, 100, (const uint8_t *)payload, 10, 0) ==
              0);
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.segments_current == 1);
        CHECK(metrics.segments_peak == 1);
        CHECK(metrics.bytes_current == 10);
        CHECK(metrics.bytes_peak == 10);
        CHECK(metrics.accepted_segments == 1);
        CHECK(metrics.accepted_bytes == 10);
        CHECK(metrics.reorder_distance_max == 10);

        /* Advancing into the node exercises trim followed by a full drain. */
        sk.u.tcp.recv_ack = 105;
        tcp_test_ofo_drain(&sk);
        CHECK(sk.u.tcp.ofo_count == 0);
        CHECK(sk.u.tcp.ofo_reorder_distance_peak == 0);
        CHECK(sk.u.tcp.rx_queue_head != NULL);
        CHECK(sk.u.tcp.rx_queue_head->len == 5);
        CHECK(memcmp(sk.u.tcp.rx_queue_head->data, payload + 5, 5) == 0);
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.segments_current == 0);
        CHECK(metrics.bytes_current == 0);
        CHECK(metrics.released_segments == 1);
        CHECK(metrics.released_bytes == 10);
        free_test_rx_queue(&sk);

        init_socket(&sk, 0, TCP_RCVBUF_SIZE);
        CHECK(tcp_test_ofo_insert(&sk, 20, (const uint8_t *)payload, 4, 0) ==
              0);
        tcp_test_ofo_purge(&sk);
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.released_segments == 1);
        CHECK(metrics.released_bytes == 4);
}

static void test_pressure_tier(uint32_t first_seq, uint32_t expected_limit) {
        struct nsock sk;
        struct tcp_ofo_metrics metrics;
        static const uint8_t byte = 0x5a;

        tcp_test_ofo_metrics_reset();
        tcp_test_ofo_force_pressure(true);
        init_socket(&sk, 0, TCP_RCVBUF_SIZE);
        for (uint32_t i = 0; i < expected_limit; i++)
                CHECK(tcp_test_ofo_insert(&sk, first_seq + i * 2U, &byte, 1,
                                          0) == 0);
        CHECK(tcp_test_ofo_insert(&sk, first_seq + expected_limit * 2U, &byte,
                                  1, 0) == -1);
        CHECK(sk.u.tcp.ofo_count == expected_limit);
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.segments_current == expected_limit);
        CHECK(metrics.drop_seg_limit == 1);
        CHECK(metrics.drop_pressure == 1);
        CHECK(metrics.pressure_active == 1);
        CHECK(metrics.reorder_distance_max ==
              first_seq + expected_limit * 2U);
        tcp_test_ofo_purge(&sk);
}

static void test_pressure_tiers_and_recovery(void) {
        struct nsock sk;
        struct tcp_ofo_metrics metrics;
        static const uint8_t byte = 0x33;

        test_pressure_tier(2, TCP_OFO_PRESSURE_NEAR_MAX_SEGS);
        test_pressure_tier(TCP_OFO_PRESSURE_NEAR_DISTANCE + 2U,
                           TCP_OFO_PRESSURE_MEDIUM_MAX_SEGS);
        test_pressure_tier(TCP_OFO_PRESSURE_MEDIUM_DISTANCE + 2U,
                           TCP_OFO_PRESSURE_FAR_MAX_SEGS);

        tcp_test_ofo_metrics_reset();
        tcp_test_ofo_force_pressure(true);
        init_socket(&sk, 0, TCP_RCVBUF_SIZE);
        for (uint32_t i = 0; i < TCP_OFO_PRESSURE_NEAR_MAX_SEGS; i++)
                CHECK(tcp_test_ofo_insert(&sk, 2U + i * 2U, &byte, 1, 0) ==
                      0);
        tcp_test_ofo_force_pressure(false);
        CHECK(sk.u.tcp.ofo_count == TCP_OFO_PRESSURE_NEAR_MAX_SEGS);
        for (uint32_t i = TCP_OFO_PRESSURE_NEAR_MAX_SEGS;
             i < TCP_OFO_MAX_SEGS; i++)
                CHECK(tcp_test_ofo_insert(&sk, 2U + i * 2U, &byte, 1, 0) ==
                      0);
        CHECK(sk.u.tcp.ofo_count == TCP_OFO_MAX_SEGS);
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.pressure_active == 0);
        CHECK(metrics.pressure_transitions == 2);
        tcp_test_ofo_purge(&sk);
}

static void test_owner_byte_pressure_hysteresis(void) {
        struct nsock sk;
        struct tcp_ofo_metrics metrics;
        uint8_t payload[25] = {0};

        tcp_test_ofo_metrics_reset();
        tcp_test_ofo_set_owner_limit(100);
        tcp_test_ofo_use_auto_pressure();
        init_socket(&sk, 0, TCP_RCVBUF_SIZE);
        CHECK(tcp_test_ofo_insert(&sk, 2, payload, sizeof(payload), 0) == 0);
        CHECK(tcp_test_ofo_insert(&sk, 40, payload, sizeof(payload), 0) == 0);
        CHECK(tcp_test_ofo_insert(&sk, 80, payload, sizeof(payload), 0) == 0);
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.bytes_current == 75);
        CHECK(metrics.pressure_active == 1);
        CHECK(metrics.pressure_transitions == 1);

        tcp_test_ofo_purge(&sk);
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.bytes_current == 0);
        CHECK(metrics.pressure_active == 0);
        CHECK(metrics.pressure_transitions == 1);
}

static void test_ofo_drop_metrics(void) {
        struct nsock sk;
        struct tcp_ofo_metrics metrics;
        static const char payload[] = "abcdefgh";

        tcp_test_ofo_metrics_reset();
        tcp_test_ofo_force_pressure(false);
        init_socket(&sk, 100, 8);
        CHECK(tcp_test_ofo_insert(&sk, 108, (const uint8_t *)payload, 1, 0) ==
              0);
        CHECK(tcp_test_ofo_insert(&sk, 90, (const uint8_t *)payload, 1, 0) ==
              0); /* duplicate: not a drop */
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.drop_rcvbuf == 1);
        CHECK(metrics.drop_seg_limit == 0);

        tcp_test_ofo_metrics_reset();
        tcp_test_ofo_force_pressure(false);
        init_socket(&sk, 0, 4);
        CHECK(tcp_test_ofo_insert(&sk, 2, (const uint8_t *)payload, 2, 0) ==
              0);
        CHECK(tcp_test_ofo_insert(&sk, 2, (const uint8_t *)payload, 2, 0) ==
              0);
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.accepted_segments == 1);
        CHECK(metrics.drop_rcvbuf == 0);
        tcp_test_ofo_purge(&sk);

        init_socket(&sk, 0, TCP_RCVBUF_SIZE);
        sk.u.tcp.ofo_bytes = TCP_OFO_PRESSURE_NEAR_MAX_BYTES;
        tcp_test_ofo_force_pressure(true);
        CHECK(tcp_test_ofo_insert(&sk, 2, (const uint8_t *)payload, 1, 0) ==
              -1);
        sk.u.tcp.ofo_bytes = 0;
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.drop_byte_limit == 1);
        CHECK(metrics.drop_pressure == 1);

        tcp_test_ofo_metrics_reset();
        tcp_test_ofo_force_pressure(false);
        tcp_test_ofo_set_owner_limit(4);
        init_socket(&sk, 0, TCP_RCVBUF_SIZE);
        CHECK(tcp_test_ofo_insert(&sk, 2, (const uint8_t *)payload, 4, 0) ==
              0);
        CHECK(tcp_test_ofo_insert(&sk, 10, (const uint8_t *)payload, 1, 0) ==
              -1);
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.drop_owner_limit == 1);
        tcp_test_ofo_purge(&sk);

        tcp_test_ofo_metrics_reset();
        tcp_test_ofo_force_pressure(false);
        init_socket(&sk, 0, TCP_RCVBUF_SIZE);
        tcp_test_ofo_fail_next_alloc();
        CHECK(tcp_test_ofo_insert(&sk, 2, (const uint8_t *)payload, 1, 0) ==
              -1);
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.drop_alloc == 1);
}

static void test_order_and_lower_bound(void) {
        struct nsock sk;
        static const char a[] = "AAAA";
        static const char b[] = "BBBB";
        static const char c[] = "CCCC";
        static const char d[] = "DDDD";

        init_socket(&sk, 0, 1024);
        CHECK(tcp_test_ofo_insert(&sk, 120, (const uint8_t *)b, 4, 0) == 0);
        CHECK(tcp_test_ofo_insert(&sk, 100, (const uint8_t *)a, 4, 0) == 0);
        CHECK(tcp_test_ofo_insert(&sk, 140, (const uint8_t *)c, 4, 0) == 0);
        CHECK(tcp_test_ofo_insert(&sk, 130, (const uint8_t *)d, 4, 0) == 0);
        CHECK(sk.u.tcp.sack_recent_valid);
        CHECK(sk.u.tcp.sack_recent.left == 130);
        CHECK(sk.u.tcp.sack_recent.right == 134);
        /* A retained duplicate still identifies its real OFO block. */
        CHECK(tcp_test_ofo_insert(&sk, 130, (const uint8_t *)d, 4, 0) == 0);
        CHECK(sk.u.tcp.sack_recent_valid);
        validate_indexes(&sk);

        expect_segment(sk.u.tcp.ofo, 100, a, 4, 0);
        expect_segment(sk.u.tcp.ofo->next, 120, b, 4, 0);
        expect_segment(sk.u.tcp.ofo->next->next, 130, d, 4, 0);
        expect_segment(sk.u.tcp.ofo_tail, 140, c, 4, 0);
        CHECK(tcp_test_ofo_lower_bound(&sk, 99)->seq == 100);
        CHECK(tcp_test_ofo_lower_bound(&sk, 120)->seq == 120);
        CHECK(tcp_test_ofo_lower_bound(&sk, 121)->seq == 130);
        CHECK(tcp_test_ofo_lower_bound(&sk, 141) == NULL);
        tcp_test_ofo_purge(&sk);
        validate_indexes(&sk);
}

static void test_overlap_trimming_and_fin(void) {
        struct nsock sk;
        static const char original[] = "0123456789";
        static const char overlap[] = "abcdefghijklmnopqrst";

        init_socket(&sk, 0, 64);
        CHECK(tcp_test_ofo_insert(&sk, 20, (const uint8_t *)original, 10, 0) ==
              0);
        CHECK(tcp_test_ofo_insert(&sk, 15, (const uint8_t *)overlap, 20, 0) ==
              0);
        validate_indexes(&sk);
        expect_segment(sk.u.tcp.ofo, 15, overlap, 5, 0);
        expect_segment(sk.u.tcp.ofo->next, 20, original, 10, 0);
        expect_segment(sk.u.tcp.ofo->next->next, 30, overlap + 15, 5, 0);

        CHECK(tcp_test_ofo_insert(&sk, 40, NULL, 0, 1) == 0);
        CHECK(tcp_test_ofo_insert(&sk, 40, NULL, 0, 1) == 0);
        CHECK(sk.u.tcp.ofo_count == 4);
        expect_segment(sk.u.tcp.ofo_tail, 40, "", 0, 1);
        tcp_test_ofo_purge(&sk);
}

static void test_receive_window_and_capacity(void) {
        struct nsock sk;
        struct tcp_ofo_metrics metrics;
        static const char payload[] = "abcdefghijklmnopqrst";
        uint32_t i;

        init_socket(&sk, 100, 32);
        CHECK(tcp_test_ofo_insert(&sk, 90, (const uint8_t *)payload, 20, 0) ==
              0);
        CHECK(tcp_test_ofo_insert(&sk, 110, (const uint8_t *)payload, 16, 0) ==
              0);
        CHECK(tcp_test_ofo_insert(&sk, 122, (const uint8_t *)payload, 1, 0) ==
              0);
        validate_indexes(&sk);
        expect_segment(sk.u.tcp.ofo, 100, payload + 10, 10, 0);
        expect_segment(sk.u.tcp.ofo_tail, 110, payload, 12, 0);
        tcp_test_ofo_purge(&sk);

        tcp_test_ofo_metrics_reset();
        tcp_test_ofo_force_pressure(false);
        init_socket(&sk, 0, TCP_RCVBUF_SIZE);
        for (i = 0; i < TCP_OFO_MAX_SEGS; i++) {
                CHECK(tcp_test_ofo_insert(&sk, i * 2, (const uint8_t *)payload,
                                          1, 0) == 0);
        }
        CHECK(tcp_test_ofo_insert(&sk, TCP_OFO_MAX_SEGS * 2,
                                  (const uint8_t *)payload, 1, 0) == -1);
        CHECK(!sk.u.tcp.sack_recent_valid);
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.segments_current == TCP_OFO_MAX_SEGS);
        CHECK(metrics.segments_peak == TCP_OFO_MAX_SEGS);
        CHECK(metrics.accepted_segments == TCP_OFO_MAX_SEGS);
        CHECK(metrics.drop_seg_limit == 1);
        CHECK(metrics.drop_pressure == 0);
        validate_indexes(&sk);
        tcp_test_ofo_purge(&sk);
}

static void test_sequence_wraparound(void) {
        struct nsock sk;
        struct tcp_ofo_metrics metrics;
        static const char a[] = "A";
        static const char b[] = "B";
        static const char c[] = "C";

        tcp_test_ofo_metrics_reset();
        tcp_test_ofo_force_pressure(false);
        init_socket(&sk, UINT_MAX - 15, 64);
        CHECK(tcp_test_ofo_insert(&sk, 0, (const uint8_t *)b, 1, 0) == 0);
        CHECK(tcp_test_ofo_insert(&sk, UINT_MAX - 7, (const uint8_t *)a, 1,
                                  0) == 0);
        CHECK(tcp_test_ofo_insert(&sk, 8, (const uint8_t *)c, 1, 0) == 0);
        validate_indexes(&sk);
        expect_segment(sk.u.tcp.ofo, UINT_MAX - 7, a, 1, 0);
        expect_segment(sk.u.tcp.ofo->next, 0, b, 1, 0);
        expect_segment(sk.u.tcp.ofo_tail, 8, c, 1, 0);
        CHECK(tcp_test_ofo_lower_bound(&sk, UINT_MAX - 6)->seq == 0);
        CHECK(tcp_test_ofo_lower_bound(&sk, 1)->seq == 8);
        tcp_ofo_metrics_take(&metrics);
        CHECK(metrics.reorder_distance_max == 24);
        tcp_test_ofo_purge(&sk);
}

static void test_teardown_stream_receive_and_eof(void) {
        struct nsock sk;
        static const uint8_t head[] = "abcd";
        static const uint8_t tail[] = "ef";
        static const uint8_t after_fin[] = "x";
        static const uint8_t oversized[] = "abcdef";

        /* FIN_WAIT_2 must retain OFO payload and deliver it in stream order
         * before publishing EOF/TIME_WAIT. */
        init_socket(&sk, 100, 64);
        sk.u.tcp.status = TCP_STATUS_FIN_WAIT_2;
        CHECK(tcp_test_receive_stream_segment(&sk, 104, tail, 2, true));
        CHECK(tcp_test_receive_stream_segment(&sk, 108, after_fin, 1,
                                              false));
        CHECK(sk.u.tcp.recv_ack == 100);
        CHECK(sk.u.tcp.ofo_count == 2);
        CHECK(!sk.u.tcp.peer_eof);
        CHECK(tcp_test_receive_stream_segment(&sk, 100, head, 4, false));
        CHECK(sk.u.tcp.recv_ack == 107);
        CHECK(sk.u.tcp.peer_eof);
        CHECK(sk.u.tcp.status == TCP_STATUS_TIME_WAIT);
        CHECK(sk.u.tcp.ofo_count == 0);
        CHECK(sk.u.tcp.rcvbuf_used == 6);
        CHECK(sk.u.tcp.rx_queue_count == 2);
        CHECK(sk.u.tcp.rx_queue_head->len == sizeof(head) - 1);
        CHECK(memcmp(sk.u.tcp.rx_queue_head->data, head, sizeof(head) - 1) ==
              0);
        CHECK(sk.u.tcp.rx_queue_tail->len == sizeof(tail) - 1);
        CHECK(memcmp(sk.u.tcp.rx_queue_tail->data, tail, sizeof(tail) - 1) ==
              0);
        free_test_rx_queue(&sk);

        /* The shared receive window clips both payload and an out-of-window
         * FIN in FIN_WAIT_1 exactly as it does in ESTABLISHED. */
        init_socket(&sk, 200, 4);
        sk.u.tcp.status = TCP_STATUS_FIN_WAIT_1;
        CHECK(tcp_test_receive_stream_segment(&sk, 200, oversized, 6, true));
        CHECK(sk.u.tcp.recv_ack == 204);
        CHECK(!sk.u.tcp.peer_eof);
        CHECK(sk.u.tcp.status == TCP_STATUS_FIN_WAIT_1);
        CHECK(sk.u.tcp.rx_queue_head->len == 4);
        CHECK(memcmp(sk.u.tcp.rx_queue_head->data, oversized, 4) == 0);
        free_test_rx_queue(&sk);

        /* A retransmission whose payload is wholly below RCV.NXT can still
         * carry the first FIN exactly at RCV.NXT. */
        init_socket(&sk, 300, 32);
        sk.u.tcp.status = TCP_STATUS_FIN_WAIT_1;
        CHECK(tcp_test_receive_stream_segment(&sk, 295, oversized, 5, true));
        CHECK(sk.u.tcp.recv_ack == 301);
        CHECK(sk.u.tcp.peer_eof);
        CHECK(sk.u.tcp.status == TCP_STATUS_CLOSING);
        CHECK(sk.u.tcp.rx_queue_count == 0);

        /* CLOSE_WAIT has already established EOF; later payload is only a
         * retransmission and must never be delivered a second time. */
        init_socket(&sk, 400, 32);
        sk.u.tcp.status = TCP_STATUS_CLOSE_WAIT;
        CHECK(tcp_test_receive_stream_segment(&sk, 400, head, 4, true));
        CHECK(sk.u.tcp.recv_ack == 400);
        CHECK(sk.u.tcp.peer_eof);
        CHECK(sk.u.tcp.rx_queue_count == 0);
}

static void test_initial_send_window_update(void) {
        struct nsock sk;
        const uint32_t peer_seq = UINT32_C(2544753203);

        memset(&sk, 0, sizeof(sk));
        /*
         * A peer ISN in the upper half of the sequence space must still
         * establish the first send-window value.  An all-zero snd_wl1 is not a
         * valid RFC 793 ordering baseline before any segment has supplied a
         * window.
         */
        tcp_test_update_snd_wnd(&sk, peer_seq, 100, 32768);
        CHECK(sk.u.tcp.snd_wnd_valid);
        CHECK(sk.u.tcp.snd_wnd == 32768);
        CHECK(sk.u.tcp.snd_wl1 == peer_seq);
        CHECK(sk.u.tcp.snd_wl2 == 100);

        /* Once initialized, an older segment must not overwrite the window. */
        tcp_test_update_snd_wnd(&sk, peer_seq - 1, 200, 1);
        CHECK(sk.u.tcp.snd_wnd == 32768);
}

static void test_chunked_send_buffer_ack_reclaim(void) {
        struct nsock sk;
        uint8_t payload[TCP_MEMORY_CHUNK_SIZE + 32];
        uint32_t available;
        const uint8_t *view;

        memset(&sk, 0, sizeof(sk));
        for (size_t i = 0; i < sizeof(payload); i++)
                payload[i] = (uint8_t)i;
        CHECK(tcp_test_sndbuf_append(&sk, 1000, payload, sizeof(payload)) ==
              (int)sizeof(payload));
        CHECK(sk.u.tcp.sndbuf.len == sizeof(payload));

        view = tcp_test_sndbuf_peek(&sk, 1000, &available);
        CHECK(view != NULL);
        CHECK(available == TCP_MEMORY_CHUNK_SIZE);
        CHECK(memcmp(view, payload, available) == 0);

        tcp_test_sndbuf_remove(&sk, TCP_MEMORY_CHUNK_SIZE + 16);
        CHECK(sk.u.tcp.sndbuf.head_seq == 1000 + TCP_MEMORY_CHUNK_SIZE + 16);
        CHECK(sk.u.tcp.sndbuf.len == 16);
        view = tcp_test_sndbuf_peek(&sk, sk.u.tcp.sndbuf.head_seq, &available);
        CHECK(view != NULL);
        CHECK(available == 16);
        CHECK(memcmp(view, payload + TCP_MEMORY_CHUNK_SIZE + 16, 16) == 0);

        tcp_test_sndbuf_remove(&sk, 16);
        CHECK(sk.u.tcp.sndbuf.head == NULL);
        CHECK(sk.u.tcp.sndbuf.tail == NULL);
        CHECK(sk.u.tcp.sndbuf.len == 0);
        tcp_test_sndbuf_free(&sk);
}

static void test_listener_child_list(void) {
        struct nsock listener;
        struct nsock first;
        struct nsock middle;
        struct nsock last;

        memset(&listener, 0, sizeof(listener));
        memset(&first, 0, sizeof(first));
        memset(&middle, 0, sizeof(middle));
        memset(&last, 0, sizeof(last));

        tcp_listener_child_attach(&listener, &first);
        tcp_listener_child_attach(&listener, &middle);
        tcp_listener_child_attach(&listener, &last);
        CHECK(listener.u.tcp.listener_child_head == &first);
        CHECK(listener.u.tcp.listener_child_tail == &last);
        CHECK(first.u.tcp.listener_child_next == &middle);
        CHECK(middle.u.tcp.listener_child_prev == &first);
        CHECK(middle.u.tcp.listener_child_next == &last);
        CHECK(last.u.tcp.listener_child_prev == &middle);

        tcp_listener_child_detach(&middle);
        CHECK(middle.u.tcp.listener == NULL);
        CHECK(middle.u.tcp.listener_child_prev == NULL);
        CHECK(middle.u.tcp.listener_child_next == NULL);
        CHECK(first.u.tcp.listener_child_next == &last);
        CHECK(last.u.tcp.listener_child_prev == &first);

        tcp_listener_child_detach(&first);
        CHECK(listener.u.tcp.listener_child_head == &last);
        CHECK(last.u.tcp.listener_child_prev == NULL);
        tcp_listener_child_detach(&last);
        CHECK(listener.u.tcp.listener_child_head == NULL);
        CHECK(listener.u.tcp.listener_child_tail == NULL);

        /* Reattachment first removes the child from its previous parent. */
        tcp_listener_child_attach(&listener, &middle);
        tcp_listener_child_attach(&first, &middle);
        CHECK(listener.u.tcp.listener_child_head == NULL);
        CHECK(listener.u.tcp.listener_child_tail == NULL);
        CHECK(first.u.tcp.listener_child_head == &middle);
        CHECK(first.u.tcp.listener_child_tail == &middle);
        tcp_listener_child_detach(&middle);
}

int main(void) {
        char *eal_argv[] = {"test_ofo", "--in-memory", "--no-pci"};

        CHECK(rte_eal_init((int)ARRAY_SIZE(eal_argv), eal_argv) >= 0);
        test_ofo_metrics_lifecycle();
        test_pressure_tiers_and_recovery();
        test_owner_byte_pressure_hysteresis();
        test_ofo_drop_metrics();
        test_order_and_lower_bound();
        test_overlap_trimming_and_fin();
        test_receive_window_and_capacity();
        test_sequence_wraparound();
        test_teardown_stream_receive_and_eof();
        test_initial_send_window_update();
        test_chunked_send_buffer_ack_reclaim();
        test_listener_child_list();
        puts("test_ofo: PASS");
        return EXIT_SUCCESS;
}
