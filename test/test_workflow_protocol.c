/* Boundary tests: strict JSON, decoded scalar extraction, split HTTP framing,
 * explicit non-2xx business responses, CNAME matching, and startup rejection.
 */
#include "../traffic-gen/core/txn.h"
#include "../traffic-gen/core/workflow_plan.h"
#include "../traffic-gen/proto/dns/dns_client.h"
#include "../traffic-gen/proto/http/http_client.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Checks typed scalars, JSON Pointer escapes, Unicode, and strict rejection.
 * Invalid input must fail before it can become a rendered request. */
static void values(void) {
        struct tg_document d;
        const char *json = "{\"a/b\":[0,\"quote\\\" \\u732b "
                           "\\ud83d\\ude00\"],\"false\":false}";
        assert(tg_document_parse(&d, json, strlen(json)) == 0);
        struct tg_value v;
        assert(tg_value_read(&d.view, tg_json_pointer(&d.view, "/a~1b/1"),
                             &v) == 0);
        assert(!strcmp(v.text, "quote\" 猫 😀"));
        assert(tg_json_pointer(&d.view, "/a~1b/01") < 0);
        assert(tg_value_read(&d.view, tg_json_pointer(&d.view, "/false"), &v) ==
                   0 &&
               v.kind == TG_VALUE_BOOL);
        tg_document_free(&d);
        const char invalid_utf8[] = {'"', (char)0xc0, (char)0x80, '"'};
        assert(tg_document_parse(&d, invalid_utf8, sizeof(invalid_utf8)) < 0);
        const char *bad[] = {
            "{\"a\" 1}",    "[1 2]",   "[1,]",  "[01]",
            "[NaN]",        "[truex]", "{} {}", "{\"x\":\"\\ud800\"}",
            "[\"\\u0000\"]"};
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
                assert(tg_document_parse(&d, bad[i], strlen(bad[i])) < 0);
}
/* Feeds one byte at a time to exercise every Header/chunk boundary.
 * A parsed 404 may be accepted by business policy; body capture overflow must
 * fail explicitly rather than exposing truncated JSON. */
static void http(void) {
        struct tg_http_config cfg = tg_http_bootstrap_config;
        cfg.accept_any_status = true;
        cfg.capture_headers = true;
        cfg.capture_json = true;
        struct tg_txn txn;
        assert(tg_txn_init(&txn, &tg_http_proto_ops, &cfg) == 0);
        /* Informational headers must not finish the exchange or leak into
         * the final response's duplicate-header indices. */
        const char *interim = "HTTP/1.1 103 Early Hints\r\nX-Key: interim\r\n\r\n";
        assert(tg_txn_on_rx(&txn, (const uint8_t *)interim, strlen(interim)) ==
               TG_PROTO_MORE);
        const char *response = "HTTP/1.1 404 Not Found\r\nTransfer-Encoding: "
                               "chunked\r\nX-Empty:\r\nX-Key: first\r\nx-key: "
                               "second\r\n\r\n7\r\n{\"x\":1}\r\n0\r\n\r\n";
        enum tg_proto_result result = TG_PROTO_MORE;
        for (size_t i = 0; i < strlen(response); i++) {
                result = tg_txn_on_rx(&txn, (const uint8_t *)response + i, 1);
                assert(result == (i + 1 == strlen(response) ? TG_PROTO_COMPLETE
                                                            : TG_PROTO_MORE));
        }
        struct tg_value v;
        assert(tg_http_proto_ops.export_value(&txn, "status", "", 0, &v) == 0 &&
               !strcmp(v.text, "404"));
        assert(tg_http_proto_ops.export_value(&txn, "header", "X-Key", 1, &v) ==
                   0 &&
               !strcmp(v.text, "second"));
        assert(tg_http_proto_ops.export_value(&txn, "header", "x-empty", 0,
                                              &v) == 0 &&
               !strcmp(v.text, ""));
        assert(tg_http_proto_ops.export_value(&txn, "json", "/x", 0, &v) == 0 &&
               v.kind == TG_VALUE_NUMBER && !strcmp(v.text, "1"));
        assert(tg_http_proto_ops.export_value(&txn, "json", "/missing", 0, &v) <
               0);
        tg_txn_reset(&txn);
        assert(tg_txn_init(&txn, &tg_http_proto_ops, &cfg) == 0);
        const char *head = "HTTP/1.1 200 OK\r\nContent-Length: 65537\r\n\r\n";
        assert(tg_txn_on_rx(&txn, (const uint8_t *)head, strlen(head)) ==
               TG_PROTO_MORE);
        uint8_t *large = calloc(65537, 1);
        assert(large);
        assert(tg_txn_on_rx(&txn, large, 65537) == TG_PROTO_FAILED);
        free(large);
        tg_txn_reset(&txn);
        strcpy(cfg.method, "HEAD");
        assert(tg_txn_init(&txn, &tg_http_proto_ops, &cfg) == 0);
        assert(tg_txn_on_rx(&txn, (const uint8_t *)head, strlen(head)) ==
               TG_PROTO_COMPLETE);
        tg_txn_reset(&txn);
        strcpy(cfg.method, "GET");
        assert(tg_txn_init(&txn, &tg_http_proto_ops, &cfg) == 0);
        const char *upgrade = "HTTP/1.1 101 Switching Protocols\r\n\r\n";
        assert(tg_txn_on_rx(&txn, (const uint8_t *)upgrade, strlen(upgrade)) ==
               TG_PROTO_FAILED);
        tg_txn_reset(&txn);
}
static void put16(uint8_t *p, unsigned n) {
        p[0] = (uint8_t)(n >> 8);
        p[1] = (uint8_t)n;
}
/* Builds a CNAME chain and changes only the terminal A-record owner.
 * The second packet stays structurally valid but must not select an address. */
static void dns(void) {
        struct tg_dns_config cfg = {.qname = "alias.test",
                                    .qtype = 1,
                                    .transaction_id = 1,
                                    .require_address = true};
        struct tg_txn txn;
        uint8_t wire[512];
        size_t n;
        assert(tg_dns_proto_ops.build_request(&cfg, wire, sizeof(wire), &n) ==
               0);
        put16(wire + 2, 0x8180);
        put16(wire + 6, 2);
        uint8_t cname[] = {0xc0, 0x0c, 0,   5,   0,   1,   0,   0,   0,
                           30,   0,    13,  6,   't', 'a', 'r', 'g', 'e',
                           't',  4,    't', 'e', 's', 't', 0};
        memcpy(wire + n, cname, sizeof(cname));
        n += sizeof(cname);
        uint8_t address[] = {6,   't', 'a', 'r', 'g', 'e', 't', 4, 't',
                             'e', 's', 't', 0,   0,   1,   0,   1, 0,
                             0,   0,   30,  0,   4,   192, 0,   2, 1};
        memcpy(wire + n, address, sizeof(address));
        n += sizeof(address);
        assert(tg_txn_init(&txn, &tg_dns_proto_ops, &cfg) == 0);
        assert(tg_txn_on_rx(&txn, wire, n) == TG_PROTO_COMPLETE);
        struct tg_value v;
        assert(tg_dns_proto_ops.export_value(&txn, "address", "", 0, &v) == 0 &&
               !strcmp(v.text, "192.0.2.1"));
        tg_txn_reset(&txn);
        /* Same valid packet framing, but no A record belongs to the CNAME
         * target. */
        wire[n - sizeof(address) + 1] = 'z';
        assert(tg_txn_init(&txn, &tg_dns_proto_ops, &cfg) == 0);
        assert(tg_txn_on_rx(&txn, wire, n) == TG_PROTO_FAILED);
        tg_txn_reset(&txn);
}
/* Accepts the first plan; rejects self-jumps, duplicate names, and file data
 * references that bypass launcher expansion. Successful plans are released. */
static void compiler(void) {
        /* Escaped names/selectors must use decoded lookup semantics, while
         * an unrelated request header named source must not enable capture. */
        const char *escaped =
            "{\"vars\":{\"ta\\u006be\":true},\"st\\u0065ps\":["
            "{\"name\":\"plain\",\"type\":\"http\",\"peer\":{\"ip\":\"192.0.2.1\"},"
            "\"http\":{\"headers\":{\"source\":\"json\"}}},"
            "{\"name\":\"capture\",\"type\":\"http\",\"peer\":{\"ip\":\"192.0.2.1\"},"
            "\"http\":{},\"extract\":{\"token\":{\"source\":\"j\\u0073on\",\"path\":\"/token\"}},"
            "\"checks\":[{\"left\":{\"source\":\"he\\u0061der\",\"path\":\"X\"},\"op\":\"exists\"}]}]}";
        struct tg_document input;
        void *compiled = NULL;
        assert(tg_document_parse(&input, escaped, strlen(escaped)) == 0);
        assert(tg_workflow_compile(&input.view, 0, &compiled) == 0);
        tg_document_free(&input);
        const struct tg_workflow_plan *wf = compiled;
        assert(!wf->steps[0].capture_json && !wf->steps[0].capture_headers);
        assert(wf->steps[1].capture_json && wf->steps[1].capture_headers);
        struct tg_value v;
        assert(tg_value_read(&wf->doc.view, tg_wf_member(wf, wf->vars, "take"), &v) == 0);
        assert(v.kind == TG_VALUE_BOOL && !strcmp(v.text, "true"));
        tg_workflow_proto_ops.config_free(compiled);
        const char *cases[] = {
            "{\"steps\":[{\"name\":\"pause\",\"type\":\"think\",\"ms\":2}]}",
            "{\"steps\":[{\"name\":\"pause\",\"type\":\"think\",\"ms\":2,"
            "\"next\":\"pause\"}]}",
            "{\"steps\":[{\"name\":\"x\",\"type\":\"think\"},{\"name\":\"x\","
            "\"type\":\"think\"}]}",
            "{\"dataset\":{\"file\":\"unexpanded.csv\"},\"steps\":[{\"name\":"
            "\"pause\",\"type\":\"think\"}]}"};
        for (unsigned i = 0; i < 4; i++) {
                struct tg_document d;
                void *p = NULL;
                assert(tg_document_parse(&d, cases[i], strlen(cases[i])) == 0);
                int rc = tg_workflow_compile(&d.view, 0, &p);
                assert(i == 0 ? rc == 0 : rc < 0);
                if (p)
                        tg_workflow_proto_ops.config_free(p);
                tg_document_free(&d);
        }
}
int main(void) {
        values();
        http();
        dns();
        compiler();
        puts("PASS: workflow JSON/protocol/compiler boundaries");
}
