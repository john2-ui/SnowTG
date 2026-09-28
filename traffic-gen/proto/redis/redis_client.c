#include "redis_client.h"
#include "../../core/txn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TG_REDIS_BULK_MAX (UINT64_C(512) * 1024 * 1024)

enum redis_parse_state { TYPE, LINE, LINE_LF, BODY, BODY_CR, BODY_LF, DONE, FAILED };
struct tg_redis_state {
        enum redis_parse_state state;
        uint64_t remaining;
        size_t line_len;
        uint8_t type;
        /* Only response headers are retained, never GET payloads. */
        char line[1024];
};

static int config_clone(const void *source, void **destination) {
        if (!source || !destination) {
                errno = EINVAL;
                return -1;
        }
        struct tg_redis_config *copy = malloc(sizeof(*copy));
        if (!copy)
                return -1;
        *copy = *(const struct tg_redis_config *)source;
        *destination = copy;
        return 0;
}

static bool config_valid(const struct tg_redis_config *config) {
        return config && config->command >= TG_REDIS_PING &&
               config->command <= TG_REDIS_SET &&
               memchr(config->key, 0, sizeof(config->key)) &&
               memchr(config->value, 0, sizeof(config->value));
}

static int build_request(const void *class_config, uint8_t *buffer, size_t cap,
                          size_t *length) {
        const struct tg_redis_config *config = class_config;
        static const char *const commands[] = {"PING", "GET", "SET"};
        if (!config_valid(config) || !buffer || !length) {
                errno = EINVAL;
                return -1;
        }
        const char *args[] = {commands[config->command], config->key, config->value};
        unsigned count = (unsigned)config->command + 1;
        size_t offset = 4;
        if (cap < offset)
                goto too_large;
        memcpy(buffer, "*1\r\n", offset);
        buffer[1] = (uint8_t)('0' + count);
        for (unsigned i = 0; i < count; i++) {
                char header[32];
                size_t n = strlen(args[i]);
                int h = snprintf(header, sizeof(header), "$%zu\r\n", n);
                if (h < 0 || (size_t)h >= sizeof(header) ||
                    (size_t)h + n + 2 > cap - offset)
                        goto too_large;
                memcpy(buffer + offset, header, (size_t)h);
                offset += (size_t)h;
                memcpy(buffer + offset, args[i], n);
                offset += n;
                memcpy(buffer + offset, "\r\n", 2);
                offset += 2;
        }
        *length = offset;
        return 0;
too_large:
        errno = EMSGSIZE;
        return -1;
}

static int init(struct tg_txn *txn) {
        if (!config_valid(txn->class_config)) {
                errno = EINVAL;
                return -1;
        }
        txn->proto_ctx = calloc(1, sizeof(struct tg_redis_state));
        return txn->proto_ctx ? 0 : -1;
}

static enum tg_proto_result fail(struct tg_txn *txn, enum tg_error_reason reason) {
        struct tg_redis_state *s = txn->proto_ctx;
        s->state = FAILED;
        txn->connection_reusable = false;
        txn->error_reason = reason;
        return TG_PROTO_FAILED;
}

/** Finish a bounded RESP header, leaving bulk bytes to the streaming parser. */
static int finish_line(struct tg_txn *txn) {
        struct tg_redis_state *s = txn->proto_ctx;
        const struct tg_redis_config *config = txn->class_config;
        if (s->type == '-') {
                if (!s->line_len)
                        return -1;
                txn->error_reason = TG_ERROR_REDIS_ERROR;
                return -1;
        }
        if (s->type == '+') {
                const char *expected = config->command == TG_REDIS_PING ? "PONG" : "OK";
                if (config->command == TG_REDIS_GET ||
                    s->line_len != strlen(expected) ||
                    memcmp(s->line, expected, s->line_len))
                        return -1;
                s->state = DONE;
                return 0;
        }
        if (config->command != TG_REDIS_GET || !s->line_len)
                return -1;
        if (s->line_len == 2 && !memcmp(s->line, "-1", 2)) {
                s->state = DONE;
                return 0;
        }
        for (size_t i = 0; i < s->line_len; i++) {
                unsigned digit = (unsigned char)s->line[i] - '0';
                if (digit > 9 || s->remaining > (TG_REDIS_BULK_MAX - digit) / 10)
                        return -1;
                s->remaining = s->remaining * 10 + digit;
        }
        s->state = s->remaining ? BODY : BODY_CR;
        return 0;
}

static enum tg_proto_result on_rx(struct tg_txn *txn, const uint8_t *data, size_t len) {
        struct tg_redis_state *s = txn->proto_ctx;
        const struct tg_redis_config *config = txn->class_config;
        if (!s || (!data && len))
                return TG_PROTO_FAILED;
        if (s->state == FAILED)
                return TG_PROTO_FAILED;
        size_t offset = 0;
        while (offset < len) {
                if (s->state == BODY) {
                        size_t n = len - offset;
                        if (n > s->remaining)
                                n = (size_t)s->remaining;
                        offset += n;
                        s->remaining -= n;
                        if (!s->remaining)
                                s->state = BODY_CR;
                        continue;
                }
                uint8_t c = data[offset++];
                switch (s->state) {
                case TYPE:
                        if (c != '+' && c != '-' && c != '$')
                                goto invalid;
                        s->type = c;
                        s->state = LINE;
                        break;
                case LINE:
                        if (c == '\r') {
                                s->state = LINE_LF;
                        } else {
                                if (!c || c == '\n' || s->line_len == sizeof(s->line))
                                        goto invalid;
                                s->line[s->line_len++] = (char)c;
                        }
                        break;
                case LINE_LF:
                        if (c != '\n' || finish_line(txn))
                                goto invalid;
                        break;
                case BODY_CR:
                        if (c != '\r')
                                goto invalid;
                        s->state = BODY_LF;
                        break;
                case BODY_LF:
                        if (c != '\n')
                                goto invalid;
                        s->state = DONE;
                        break;
                default:
                        /* No pipeline: even another complete reply is invalid. */
                        goto invalid;
                }
        }
        if (s->state == DONE) {
                txn->connection_reusable = config->keepalive;
                return TG_PROTO_COMPLETE;
        }
        return TG_PROTO_MORE;
invalid:
        return fail(txn, txn->error_reason == TG_ERROR_REDIS_ERROR
                             ? TG_ERROR_REDIS_ERROR : TG_ERROR_PARSE);
}

static enum tg_proto_result on_eof(struct tg_txn *txn) {
        struct tg_redis_state *s = txn->proto_ctx;
        txn->connection_reusable = false;
        return s && s->state == DONE ? TG_PROTO_COMPLETE : TG_PROTO_FAILED;
}

static void reset(struct tg_txn *txn) {
        free(txn->proto_ctx);
        txn->proto_ctx = NULL;
}

const struct tg_proto_ops tg_redis_proto_ops = {
    .name = "redis",
    .config_clone = config_clone,
    .config_free = free,
    .init = init,
    .build_request = build_request,
    .on_rx = on_rx,
    .on_eof = on_eof,
    .reset = reset,
};
