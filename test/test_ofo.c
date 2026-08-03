#ifndef TCP_TESTING
#define TCP_TESTING
#endif

#include "../pro-stack/config.h"
#include "../pro-stack/socket.h"
#include "../pro-stack/socket_owner_internal.h"
#include "../pro-stack/tcp.h"

#include <limits.h>
#include <rte_eal.h>
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

        init_socket(&sk, 0, TCP_RCVBUF_SIZE);
        for (i = 0; i < TCP_OFO_MAX_SEGS; i++) {
                CHECK(tcp_test_ofo_insert(&sk, i * 2, (const uint8_t *)payload,
                                          1, 0) == 0);
        }
        CHECK(tcp_test_ofo_insert(&sk, TCP_OFO_MAX_SEGS * 2,
                                  (const uint8_t *)payload, 1, 0) == -1);
        validate_indexes(&sk);
        tcp_test_ofo_purge(&sk);
}

static void test_sequence_wraparound(void) {
        struct nsock sk;
        static const char a[] = "A";
        static const char b[] = "B";
        static const char c[] = "C";

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
        tcp_test_ofo_purge(&sk);
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

int main(void) {
        char *eal_argv[] = {"test_ofo", "--in-memory", "--no-pci"};

        CHECK(rte_eal_init((int)ARRAY_SIZE(eal_argv), eal_argv) >= 0);
        test_order_and_lower_bound();
        test_overlap_trimming_and_fin();
        test_receive_window_and_capacity();
        test_sequence_wraparound();
        test_initial_send_window_update();
        puts("test_ofo: PASS");
        return EXIT_SUCCESS;
}
