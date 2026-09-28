#include "../traffic-gen/proto/redis/redis_client.h"
#include "../traffic-gen/proto/redis/redis_scenario.h"
#include "../traffic-gen/core/scenario.h"
#include "../traffic-gen/core/txn.h"
#include "../traffic-gen/core/value.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct tg_redis_config config = {.keepalive = true};

static void response(enum tg_redis_command command, const void *bytes, size_t n,
                     enum tg_error_reason reason) {
        config.command = command;
        /* Every split point plus byte-at-a-time delivery, including a split CRLF. */
        for (size_t split = 0; split <= n; split++) {
                struct tg_txn txn;
                assert(tg_txn_init(&txn, &tg_redis_proto_ops, &config) == 0);
                enum tg_proto_result result = tg_txn_on_rx(&txn, bytes, split);
                if (result != TG_PROTO_FAILED && split < n)
                        result = tg_txn_on_rx(&txn, (const uint8_t *)bytes + split, n - split);
                assert(result == (reason ? TG_PROTO_FAILED : TG_PROTO_COMPLETE));
                assert(txn.error_reason == reason);
                assert(txn.connection_reusable == !reason);
                if (!reason) {
                        assert(tg_txn_on_rx(&txn, NULL, 0) == TG_PROTO_COMPLETE);
                        assert(tg_txn_on_eof(&txn) == TG_PROTO_COMPLETE);
                        assert(!txn.connection_reusable);
                }
                tg_txn_reset(&txn);
        }
        struct tg_txn txn;
        assert(tg_txn_init(&txn, &tg_redis_proto_ops, &config) == 0);
        enum tg_proto_result result = TG_PROTO_MORE;
        for (size_t i = 0; i < n && result != TG_PROTO_FAILED; i++)
                result = tg_txn_on_rx(&txn, (const uint8_t *)bytes + i, 1);
        assert(result == (reason ? TG_PROTO_FAILED : TG_PROTO_COMPLETE));
        assert(txn.error_reason == reason);
        tg_txn_reset(&txn);
}

static void text_response(enum tg_redis_command command, const char *text,
                          enum tg_error_reason reason) {
        response(command, text, strlen(text), reason);
}

static int compile(const char *json, struct tg_class_plan *cls) {
        struct tg_document document;
        memset(cls, 0, sizeof(*cls));
        if (tg_document_parse(&document, json, strlen(json)))
                return -1;
        int rc = tg_redis_scenario_compile(&document.view, 0, cls);
        tg_document_free(&document);
        return rc;
}

int main(void) {
        text_response(TG_REDIS_PING, "+PONG\r\n", TG_ERROR_NONE);
        text_response(TG_REDIS_SET, "+OK\r\n", TG_ERROR_NONE);
        text_response(TG_REDIS_GET, "$-1\r\n", TG_ERROR_NONE);
        text_response(TG_REDIS_GET, "$0\r\n\r\n", TG_ERROR_NONE);
        const char binary[] = "$5\r\na\0\r\nb\r\n";
        response(TG_REDIS_GET, binary, sizeof(binary) - 1, TG_ERROR_NONE);
        for (unsigned c = TG_REDIS_PING; c <= TG_REDIS_SET; c++)
                text_response(c, "-WRONGTYPE Operation against a key\r\n", TG_ERROR_REDIS_ERROR);
        const char *bad[] = {"+OK\r\n", ":1\r\n", "*0\r\n", "$-2\r\n", "$+1\r\nx\r\n",
            "$\r\n", "$1a\r\n", "$536870913\r\n", "$18446744073709551616\r\n",
            "$1\nx\r\n", "$1\rx", "$1\r\nxXX", "$0\r\n\r\nx", "$-1\r\n$-1\r\n", "-\r\n"};
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
                text_response(TG_REDIS_GET, bad[i], TG_ERROR_PARSE);
        text_response(TG_REDIS_PING, "+OK\r\n", TG_ERROR_PARSE);
        text_response(TG_REDIS_SET, "+PONG\r\n", TG_ERROR_PARSE);
        text_response(TG_REDIS_SET, "$-1\r\n", TG_ERROR_PARSE);
        const char nul_header[] = "$1\0\r\nx\r\n";
        response(TG_REDIS_GET, nul_header, sizeof(nul_header) - 1, TG_ERROR_PARSE);

        struct tg_txn txn;
        config.command = TG_REDIS_GET;
        const char *truncated[] = {"", "$", "$1\r", "$1\r\n", "$1\r\nx", "$1\r\nx\r"};
        for (size_t i = 0; i < sizeof(truncated) / sizeof(truncated[0]); i++) {
                assert(tg_txn_init(&txn, &tg_redis_proto_ops, &config) == 0);
                assert(tg_txn_on_rx(&txn, (const uint8_t *)truncated[i], strlen(truncated[i])) == TG_PROTO_MORE);
                assert(tg_txn_on_eof(&txn) == TG_PROTO_FAILED);
                assert(txn.error_reason == TG_ERROR_PEER_EOF);
                tg_txn_reset(&txn);
        }
        /* The full 512 MiB ceiling is streamed through one small reusable buffer. */
        assert(tg_txn_init(&txn, &tg_redis_proto_ops, &config) == 0);
        assert(tg_txn_on_rx(&txn, (const uint8_t *)"$536870912\r\n", 12) == TG_PROTO_MORE);
        uint8_t chunk[8192] = {0};
        for (unsigned i = 0; i < 65536; i++)
                assert(tg_txn_on_rx(&txn, chunk, sizeof(chunk)) == TG_PROTO_MORE);
        assert(tg_txn_on_rx(&txn, (const uint8_t *)"\r\n", 2) == TG_PROTO_COMPLETE);
        assert(txn.response_bytes == UINT64_C(536870926));
        tg_txn_reset(&txn);
        memset(chunk, 'x', sizeof(chunk));
        chunk[0] = '-';
        assert(tg_txn_init(&txn, &tg_redis_proto_ops, &config) == 0);
        assert(tg_txn_on_rx(&txn, chunk, sizeof(chunk)) == TG_PROTO_FAILED);
        assert(txn.error_reason == TG_ERROR_PARSE);
        tg_txn_reset(&txn);

        struct tg_class_plan cls;
        assert(compile("{\"command\":\"pInG\"}", &cls) == 0);
        assert(cls.request_template_len == 14);
        assert(!memcmp(cls.request_template, "*1\r\n$4\r\nPING\r\n", 14));
        struct tg_redis_config *copy;
        void *cloned = NULL;
        assert(cls.proto->config_clone(cls.proto_config, &cloned) == 0);
        copy = cloned;
        cls.proto->config_free(cls.proto_config);
        assert(copy->keepalive && copy->command == TG_REDIS_PING);
        assert(tg_txn_init(&txn, &tg_redis_proto_ops, copy) == 0);
        tg_txn_reset(&txn);
        free(copy);
        assert(compile("{\"command\":\"GET\",\"key\":\"\"}", &cls) == 0);
        const char get[] = "*2\r\n$3\r\nGET\r\n$0\r\n\r\n";
        assert(cls.request_template_len == sizeof(get)-1);
        assert(!memcmp(cls.request_template, get, sizeof(get)-1));
        cls.proto->config_free(cls.proto_config);
        assert(compile("{\"command\":\"SET\",\"key\":\"\\u96ea\\r\\n\",\"value\":\"\",\"keepalive\":false}", &cls) == 0);
        const char set[] = "*3\r\n$3\r\nSET\r\n$5\r\n\xe9\x9b\xaa\r\n\r\n$0\r\n\r\n";
        assert(cls.request_template_len == sizeof(set)-1);
        assert(!memcmp(cls.request_template, set, sizeof(set)-1));
        assert(tg_txn_init(&txn, cls.proto, cls.proto_config) == 0);
        assert(tg_txn_on_rx(&txn, (const uint8_t *)"+OK\r\n", 5) == TG_PROTO_COMPLETE);
        assert(!txn.connection_reusable && !txn.peer_closes);
        tg_txn_reset(&txn);
        cls.proto->config_free(cls.proto_config);

        const char *invalid[] = {"{}", "[]", "{\"command\":42}", "{\"command\":\"INCR\"}",
            "{\"command\":\"PING\",\"key\":\"x\"}", "{\"command\":\"PING\",\"value\":\"x\"}",
            "{\"command\":\"GET\"}", "{\"command\":\"GET\",\"key\":null}",
            "{\"command\":\"GET\",\"key\":\"x\",\"value\":\"x\"}",
            "{\"command\":\"SET\",\"key\":\"x\"}",
            "{\"command\":\"GET\",\"key\":\"\\u0000\"}",
            "{\"command\":\"PING\",\"keepalive\":1}", "{\"command\":\"PING\",\"auth\":\"x\"}"};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
                assert(compile(invalid[i], &cls) == -1);
                assert(!cls.proto_config);
        }
        char json[1200], value[999];
        memset(value, 'v', 997);
        value[997] = 0;
        snprintf(json, sizeof(json), "{\"command\":\"SET\",\"key\":\"\",\"value\":\"%s\"}", value);
        assert(compile(json, &cls) == 0);
        assert(cls.request_template_len == TG_PLAN_REQUEST_TEMPLATE_CAP);
        cls.proto->config_free(cls.proto_config);
        value[997] = 'v'; value[998] = 0;
        snprintf(json, sizeof(json), "{\"command\":\"SET\",\"key\":\"\",\"value\":\"%s\"}", value);
        assert(compile(json, &cls) == -1 && errno == EMSGSIZE);
        assert(tg_txn_resource_snapshot().current == 0);
        puts("PASS: Redis RESP2 framing, streaming, errors, config and resource cleanup");
        return 0;
}
