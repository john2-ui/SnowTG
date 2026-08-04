#include "../traffic-gen/core/txn.h"
#include "../traffic-gen/proto/http/http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(condition)                                                 \
        do {                                                                   \
                if (!(condition)) {                                            \
                        fprintf(stderr, "%s:%d assertion failed: %s\n",        \
                                __FILE__, __LINE__, #condition);               \
                        return -1;                                             \
                }                                                              \
        } while (0)

static int tg_test_txn_init(struct tg_txn *txn) {
        ASSERT_TRUE(tg_txn_init(txn, &tg_http_proto_ops,
                                &tg_http_bootstrap_config) == 0);
        return 0;
}

static int test_fragmented_content_length(void) {
        static const char *const fragments[] = {
            "HTTP/1.1 2", "00 OK\r\nContent-L", "ength: 5\r\n", "\r\nhe", "llo",
        };
        struct tg_txn txn;
        size_t i;

        if (tg_test_txn_init(&txn) != 0)
                return -1;

        for (i = 0; i < sizeof(fragments) / sizeof(fragments[0]); i++) {
                enum tg_proto_result result = tg_txn_on_rx(
                    &txn, (const uint8_t *)fragments[i], strlen(fragments[i]));

                if (i + 1U == sizeof(fragments) / sizeof(fragments[0]))
                        ASSERT_TRUE(result == TG_PROTO_COMPLETE);
                else
                        ASSERT_TRUE(result == TG_PROTO_MORE);
        }

        ASSERT_TRUE(txn.response_bytes == 43U);
        tg_txn_reset(&txn);
        return 0;
}

static int test_chunked_response(void) {
        static const char first[] =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWi";
        static const char second[] = "ki\r\n5\r\npedia\r\n0\r\n\r\n";
        struct tg_txn txn;

        if (tg_test_txn_init(&txn) != 0)
                return -1;

        ASSERT_TRUE(tg_txn_on_rx(&txn, (const uint8_t *)first,
                                 sizeof(first) - 1U) == TG_PROTO_MORE);
        ASSERT_TRUE(tg_txn_on_rx(&txn, (const uint8_t *)second,
                                 sizeof(second) - 1U) == TG_PROTO_COMPLETE);
        tg_txn_reset(&txn);
        return 0;
}

static int test_reject_non_2xx(void) {
        static const char response[] =
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        struct tg_txn txn;

        if (tg_test_txn_init(&txn) != 0)
                return -1;

        ASSERT_TRUE(tg_txn_on_rx(&txn, (const uint8_t *)response,
                                 sizeof(response) - 1U) == TG_PROTO_FAILED);
        tg_txn_reset(&txn);
        return 0;
}

static int test_reject_malformed_content_length(void) {
        static const char response[] =
            "HTTP/1.1 200 OK\r\nContent-Length: invalid\r\n\r\n";
        struct tg_txn txn;

        if (tg_test_txn_init(&txn) != 0)
                return -1;

        ASSERT_TRUE(tg_txn_on_rx(&txn, (const uint8_t *)response,
                                 sizeof(response) - 1U) == TG_PROTO_FAILED);
        tg_txn_reset(&txn);
        return 0;
}

static int test_eof_delimited_body(void) {
        static const char response[] =
            "HTTP/1.0 200 OK\r\nConnection: close\r\n\r\npayload";
        struct tg_txn txn;

        if (tg_test_txn_init(&txn) != 0)
                return -1;

        ASSERT_TRUE(tg_txn_on_rx(&txn, (const uint8_t *)response,
                                 sizeof(response) - 1U) == TG_PROTO_MORE);
        ASSERT_TRUE(tg_txn_on_eof(&txn) == TG_PROTO_COMPLETE);
        tg_txn_reset(&txn);
        return 0;
}

static int test_reject_truncated_content_length(void) {
        static const char response[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhel";
        struct tg_txn txn;

        if (tg_test_txn_init(&txn) != 0)
                return -1;

        ASSERT_TRUE(tg_txn_on_rx(&txn, (const uint8_t *)response,
                                 sizeof(response) - 1U) == TG_PROTO_MORE);
        ASSERT_TRUE(tg_txn_on_eof(&txn) == TG_PROTO_FAILED);
        tg_txn_reset(&txn);
        return 0;
}

int main(void) {
        ASSERT_TRUE(test_fragmented_content_length() == 0);
        ASSERT_TRUE(test_chunked_response() == 0);
        ASSERT_TRUE(test_reject_non_2xx() == 0);
        ASSERT_TRUE(test_reject_malformed_content_length() == 0);
        ASSERT_TRUE(test_eof_delimited_body() == 0);
        ASSERT_TRUE(test_reject_truncated_content_length() == 0);

        return EXIT_SUCCESS;
}
