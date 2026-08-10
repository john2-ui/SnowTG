#include "../traffic-gen/core/txn.h"
#include "../traffic-gen/proto/dns/dns_client.h"

#include <stdint.h>
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

static uint16_t get_u16(const uint8_t *data) {
        return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void put_u16(uint8_t *data, uint16_t value) {
        data[0] = (uint8_t)(value >> 8);
        data[1] = (uint8_t)value;
}

static int build_query(const struct tg_dns_config *config, uint8_t *query,
                       size_t query_cap, size_t *query_len) {
        return tg_dns_proto_ops.build_request(config, query, query_cap,
                                              query_len);
}

static size_t build_a_response(const uint8_t *query, size_t query_len,
                               uint8_t *response, size_t response_cap,
                               uint16_t flags, uint16_t transaction_id) {
        size_t question_len = query_len - 12U - 4U;
        size_t offset = 12U;

        if (query_len < 16U || response_cap < 64U)
                return 0;
        memset(response, 0, response_cap);
        put_u16(response, transaction_id);
        put_u16(response + 2, flags);
        put_u16(response + 4, 1);
        put_u16(response + 6, 1);
        memcpy(response + offset, query + 12U, question_len + 4U);
        offset += question_len + 4U;

        response[offset++] = 0xc0;
        response[offset++] = 0x0c;
        response[offset++] = 0;
        response[offset++] = 1;
        response[offset++] = 0;
        response[offset++] = 1;
        response[offset++] = 0;
        response[offset++] = 0;
        response[offset++] = 0;
        response[offset++] = 30;
        response[offset++] = 0;
        response[offset++] = 4;
        response[offset++] = 192;
        response[offset++] = 0;
        response[offset++] = 2;
        response[offset++] = 1;
        return offset;
}

static enum tg_proto_result
parse_response(const struct tg_dns_config *config, const uint8_t *query,
               size_t query_len, const uint8_t *response, size_t response_len) {
        struct tg_txn txn;

        if (tg_txn_init_with_request(&txn, &tg_dns_proto_ops, config, query,
                                     query_len) != 0)
                return TG_PROTO_FAILED;
        enum tg_proto_result result =
            tg_txn_on_rx(&txn, response, response_len);
        tg_txn_reset(&txn);
        return result;
}

static int test_query_encoding(void) {
        const struct tg_dns_config config = {
            .qname = "www.example.com",
            .qtype = TG_DNS_QTYPE_A,
            .transaction_id = TG_DNS_TRANSACTION_ID,
        };
        uint8_t query[128] = {0};
        size_t query_len = 0;

        ASSERT_TRUE(build_query(&config, query, sizeof(query), &query_len) ==
                    0);
        ASSERT_TRUE(query_len == 33);
        ASSERT_TRUE(get_u16(query) == TG_DNS_TRANSACTION_ID);
        ASSERT_TRUE(get_u16(query + 2) == 0x0100);
        ASSERT_TRUE(get_u16(query + 4) == 1);
        ASSERT_TRUE(query[12] == 3);
        ASSERT_TRUE(memcmp(query + 13, "www", 3) == 0);
        ASSERT_TRUE(query[16] == 7);
        ASSERT_TRUE(memcmp(query + 17, "example", 7) == 0);
        ASSERT_TRUE(query[24] == 3);
        ASSERT_TRUE(memcmp(query + 25, "com", 3) == 0);
        ASSERT_TRUE(query[28] == 0);
        ASSERT_TRUE(get_u16(query + 29) == TG_DNS_QTYPE_A);
        ASSERT_TRUE(get_u16(query + 31) == 1);
        return 0;
}

static int test_aaaa_and_invalid_queries(void) {
        struct tg_dns_config config = {
            .qname = "example.com",
            .qtype = TG_DNS_QTYPE_AAAA,
            .transaction_id = TG_DNS_TRANSACTION_ID,
        };
        uint8_t query[128] = {0};
        size_t query_len = 0;

        ASSERT_TRUE(build_query(&config, query, sizeof(query), &query_len) ==
                    0);
        ASSERT_TRUE(get_u16(query + query_len - 4U) == TG_DNS_QTYPE_AAAA);
        ASSERT_TRUE(get_u16(query + query_len - 2U) == 1);

        strcpy(config.qname, "bad..name");
        ASSERT_TRUE(build_query(&config, query, sizeof(query), &query_len) ==
                    -1);
        strcpy(config.qname, "example.com.");
        ASSERT_TRUE(tg_dns_qname_validate(config.qname) == -1);
        ASSERT_TRUE(build_query(&config, query, sizeof(query), &query_len) ==
                    -1);
        strcpy(config.qname, "example.com");
        config.qtype = 15;
        ASSERT_TRUE(build_query(&config, query, sizeof(query), &query_len) ==
                    -1);
        return 0;
}

static int test_response_validation(void) {
        const struct tg_dns_config config = {
            .qname = "www.example.com",
            .qtype = TG_DNS_QTYPE_A,
            .transaction_id = TG_DNS_TRANSACTION_ID,
        };
        uint8_t query[128] = {0};
        uint8_t response[128] = {0};
        size_t query_len = 0;
        size_t response_len;

        ASSERT_TRUE(build_query(&config, query, sizeof(query), &query_len) ==
                    0);
        response_len =
            build_a_response(query, query_len, response, sizeof(response),
                             0x8180, config.transaction_id);
        ASSERT_TRUE(response_len != 0);
        ASSERT_TRUE(parse_response(&config, query, query_len, response,
                                   response_len) == TG_PROTO_COMPLETE);

        put_u16(response, (uint16_t)(config.transaction_id + 1U));
        ASSERT_TRUE(parse_response(&config, query, query_len, response,
                                   response_len) == TG_PROTO_FAILED);
        put_u16(response, config.transaction_id);
        put_u16(response + 2, 0x0100);
        ASSERT_TRUE(parse_response(&config, query, query_len, response,
                                   response_len) == TG_PROTO_FAILED);
        put_u16(response + 2, 0x8183);
        ASSERT_TRUE(parse_response(&config, query, query_len, response,
                                   response_len) == TG_PROTO_FAILED);
        put_u16(response + 2, 0x8180);
        put_u16(response + 6, 0);
        ASSERT_TRUE(parse_response(&config, query, query_len, response,
                                   response_len) == TG_PROTO_FAILED);
        put_u16(response + 6, 1);
        response[12] = 4;
        ASSERT_TRUE(parse_response(&config, query, query_len, response,
                                   response_len) == TG_PROTO_FAILED);
        response[12] = 3;
        ASSERT_TRUE(parse_response(&config, query, query_len, response,
                                   response_len - 1U) == TG_PROTO_FAILED);
        ASSERT_TRUE(parse_response(&config, query, query_len, response, 0) ==
                    TG_PROTO_FAILED);
        return 0;
}

static int test_eof_and_reset(void) {
        const struct tg_dns_config config = {
            .qname = "example.com",
            .qtype = TG_DNS_QTYPE_A,
            .transaction_id = TG_DNS_TRANSACTION_ID,
        };
        uint8_t query[128] = {0};
        size_t query_len = 0;
        struct tg_txn txn;

        ASSERT_TRUE(build_query(&config, query, sizeof(query), &query_len) ==
                    0);
        ASSERT_TRUE(tg_txn_init_with_request(&txn, &tg_dns_proto_ops, &config,
                                             query, query_len) == 0);
        ASSERT_TRUE(tg_txn_on_eof(&txn) == TG_PROTO_FAILED);
        tg_txn_reset(&txn);
        ASSERT_TRUE(txn.proto == NULL);
        ASSERT_TRUE(txn.proto_ctx == NULL);
        return 0;
}

int main(void) {
        ASSERT_TRUE(test_query_encoding() == 0);
        ASSERT_TRUE(test_aaaa_and_invalid_queries() == 0);
        ASSERT_TRUE(test_response_validation() == 0);
        ASSERT_TRUE(test_eof_and_reset() == 0);
        return EXIT_SUCCESS;
}
