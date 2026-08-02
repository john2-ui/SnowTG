#include "../pro-stack/socket.h"
#include "../pro-stack/tcp_options.h"

#include <rte_eal.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PAWS_IDLE_MS (24U * 24U * 60U * 60U * 1000U)

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,         \
              #condition);                                                     \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

static struct tcp_options_rx timestamp(uint32_t tsval) {
  struct tcp_options_rx rx;

  memset(&rx, 0, sizeof(rx));
  rx.timestamp_present = true;
  rx.tsval = tsval;
  return rx;
}

static void init_timestamp_socket(struct nsock *sk, uint32_t rcv_nxt,
                                  uint32_t ts_recent) {
  memset(sk, 0, sizeof(*sk));
  sk->u.tcp.timestamps_ok = true;
  sk->u.tcp.recv_ack = rcv_nxt;
  sk->u.tcp.ts_recent = ts_recent;
  sk->u.tcp.ts_recent_valid = true;
  sk->u.tcp.ts_recent_age_ms = tcp_options_now_ms();
}

static void test_missing_timestamp_is_rejected(void) {
  struct nsock sk;
  struct tcp_options_rx rx;

  init_timestamp_socket(&sk, 1000, 100);
  memset(&rx, 0, sizeof(rx));
  CHECK(tcp_options_process_inbound(&sk, &rx, false, 1000, 1, false, true) ==
        -1);
  CHECK(sk.u.tcp.ts_recent == 100);
}

static void test_paws_rejects_stale_timestamp(void) {
  struct nsock sk;
  struct tcp_options_rx rx = timestamp(100);

  init_timestamp_socket(&sk, 1000, 200);
  CHECK(tcp_options_process_inbound(&sk, &rx, false, 1000, 1, false, true) ==
        -2);
  CHECK(sk.u.tcp.ts_recent == 200);
}

static void test_paws_accepts_timestamp_wrap(void) {
  struct nsock sk;
  struct tcp_options_rx rx = timestamp(0x00000010);

  init_timestamp_socket(&sk, 1000, 0xfffffff0);
  CHECK(tcp_options_process_inbound(&sk, &rx, false, 1000, 1, false, true) ==
        0);
  CHECK(sk.u.tcp.ts_recent == 0x00000010);
}

static void test_left_edge_and_pure_ack_do_not_advance_recent(void) {
  struct nsock sk;
  struct tcp_options_rx old_rx = timestamp(100);
  struct tcp_options_rx new_rx = timestamp(300);

  init_timestamp_socket(&sk, 1000, 200);
  CHECK(tcp_options_process_inbound(&sk, &old_rx, false, 999, 1, false, true) ==
        0);
  CHECK(sk.u.tcp.ts_recent == 200);
  CHECK(tcp_options_process_inbound(&sk, &new_rx, false, 1000, 0, false,
                                    true) == 0);
  CHECK(sk.u.tcp.ts_recent == 200);
}

static void test_unacceptable_and_idle_segments_do_not_use_stale_recent(void) {
  struct nsock sk;
  struct tcp_options_rx rx = timestamp(100);

  init_timestamp_socket(&sk, 1000, 200);
  CHECK(tcp_options_process_inbound(&sk, &rx, false, 2000, 1, false, false) ==
        0);
  CHECK(sk.u.tcp.ts_recent == 200);

  sk.u.tcp.ts_recent_age_ms = tcp_options_now_ms() - PAWS_IDLE_MS - 1;
  CHECK(tcp_options_process_inbound(&sk, &rx, false, 1000, 1, false, true) ==
        0);
  CHECK(sk.u.tcp.ts_recent == 100);
}

static void test_rst_bypasses_paws(void) {
  struct nsock sk;
  struct tcp_options_rx rx = timestamp(100);

  init_timestamp_socket(&sk, 1000, 200);
  CHECK(tcp_options_process_inbound(&sk, &rx, true, 1000, 1, false, true) == 0);
  CHECK(sk.u.tcp.ts_recent == 200);
}

int main(void) {
  char *eal_argv[] = {"test_tcp_paws", "--in-memory", "--no-pci"};

  CHECK(rte_eal_init((int)ARRAY_SIZE(eal_argv), eal_argv) >= 0);
  test_missing_timestamp_is_rejected();
  test_paws_rejects_stale_timestamp();
  test_paws_accepts_timestamp_wrap();
  test_left_edge_and_pure_ack_do_not_advance_recent();
  test_unacceptable_and_idle_segments_do_not_use_stale_recent();
  test_rst_bypasses_paws();
  puts("test_tcp_paws: PASS");
  return EXIT_SUCCESS;
}
