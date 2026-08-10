/**
 * @file dns_client.c
 * @brief Implements the bounded DNS-over-UDP client plugin.
 */

#include "dns_client.h"

#include "../../core/txn.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct tg_dns_state {
        uint16_t transaction_id;
};

static uint16_t tg_dns_get_u16(const uint8_t *data) {
        return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void tg_dns_put_u16(uint8_t *data, uint16_t value) {
        data[0] = (uint8_t)(value >> 8);
        data[1] = (uint8_t)value;
}

static bool tg_dns_qtype_supported(uint16_t qtype) {
        return qtype == TG_DNS_QTYPE_A || qtype == TG_DNS_QTYPE_AAAA;
}

static unsigned char tg_dns_lower(unsigned char character) {
        if (character >= 'A' && character <= 'Z')
                return (unsigned char)(character - 'A' + 'a');
        return character;
}

/** @brief Checks textual qname syntax and returns its encoded wire length. */
static int tg_dns_qname_wire_length(const char *qname, size_t *length_out) {
        size_t text_length;
        size_t wire_length = 1;
        size_t label_start = 0;

        if (qname == NULL || length_out == NULL) {
                errno = EINVAL;
                return -1;
        }
        text_length = strlen(qname);
        if (text_length == 0 || text_length > 253) {
                errno = EINVAL;
                return -1;
        }
        while (label_start < text_length) {
                const char *separator = strchr(qname + label_start, '.');
                size_t label_end = separator == NULL
                                       ? text_length
                                       : (size_t)(separator - qname);
                size_t label_length = label_end - label_start;

                /*
                 * Reject empty labels and a trailing dot.  Scenario JSON uses
                 * presentation-form names without the DNS absolute-name dot.
                 */
                if (label_length == 0 || label_length > TG_DNS_LABEL_MAX ||
                    (separator != NULL && label_end + 1U == text_length) ||
                    wire_length > SIZE_MAX - label_length - 1U) {
                        errno = EINVAL;
                        return -1;
                }
                wire_length += label_length + 1U;
                if (separator == NULL)
                        break;
                label_start = label_end + 1U;
        }
        *length_out = wire_length;
        return 0;
}

/** @copydoc tg_dns_qname_validate */
int tg_dns_qname_validate(const char *qname) {
        size_t wire_length;

        return tg_dns_qname_wire_length(qname, &wire_length);
}

/** @brief Clones owning DNS class configuration for a scheduling shard. */
static int tg_dns_config_clone(const void *source, void **destination) {
        const struct tg_dns_config *source_config = source;
        struct tg_dns_config *copy;

        if (source_config == NULL || destination == NULL) {
                errno = EINVAL;
                return -1;
        }
        copy = malloc(sizeof(*copy));
        if (copy == NULL)
                return -1;
        *copy = *source_config;
        *destination = copy;
        return 0;
}

/** @brief Releases one heap-owned DNS class configuration. */
static void tg_dns_config_free(void *config) { free(config); }

/** @brief Serializes one DNS query into caller-owned storage. */
static int tg_dns_build_request(const void *class_config, uint8_t *buffer,
                                size_t buffer_cap, size_t *request_len_out) {
        const struct tg_dns_config *config = class_config;
        size_t qname_wire_length;
        size_t qname_length;
        size_t offset = 12;
        size_t label_start = 0;

        if (config == NULL || buffer == NULL || request_len_out == NULL ||
            buffer_cap < 12 ||
            tg_dns_qname_wire_length(config->qname, &qname_wire_length) != 0 ||
            !tg_dns_qtype_supported(config->qtype)) {
                errno = EINVAL;
                return -1;
        }
        qname_length = strlen(config->qname);
        if (qname_wire_length > SIZE_MAX - offset - 4U ||
            offset + qname_wire_length + 4U > buffer_cap) {
                errno = EMSGSIZE;
                return -1;
        }

        tg_dns_put_u16(buffer, config->transaction_id);
        tg_dns_put_u16(buffer + 2, UINT16_C(0x0100)); /* RD */
        tg_dns_put_u16(buffer + 4, 1);                /* QDCOUNT */
        tg_dns_put_u16(buffer + 6, 0);                /* ANCOUNT */
        tg_dns_put_u16(buffer + 8, 0);                /* NSCOUNT */
        tg_dns_put_u16(buffer + 10, 0);               /* ARCOUNT */

        while (label_start < qname_length) {
                const char *separator =
                    strchr(config->qname + label_start, '.');
                size_t label_end = separator == NULL
                                       ? qname_length
                                       : (size_t)(separator - config->qname);
                size_t label_length = label_end - label_start;

                buffer[offset++] = (uint8_t)label_length;
                memcpy(buffer + offset, config->qname + label_start,
                       label_length);
                offset += label_length;
                if (separator == NULL)
                        break;
                label_start = label_end + 1U;
        }
        buffer[offset++] = 0;
        tg_dns_put_u16(buffer + offset, config->qtype);
        offset += 2;
        tg_dns_put_u16(buffer + offset, 1); /* IN */
        offset += 2;
        *request_len_out = offset;
        return 0;
}

/**
 * Expand one possibly compressed DNS name into lowercase presentation form.
 *
 * @param next_out Offset immediately after the name at its original location.
 */
static int tg_dns_expand_name(const uint8_t *data, size_t length, size_t offset,
                              char *output, size_t output_cap,
                              size_t *next_out) {
        size_t cursor = offset;
        size_t output_length = 0;
        size_t next = offset;
        size_t jumps = 0;
        bool jumped = false;

        if (data == NULL || output == NULL || output_cap == 0 ||
            next_out == NULL || offset >= length) {
                return -1;
        }
        for (;;) {
                uint8_t label_length;

                if (cursor >= length)
                        return -1;
                label_length = data[cursor];
                if (label_length == 0) {
                        if (!jumped)
                                next = cursor + 1U;
                        if (output_length >= output_cap)
                                return -1;
                        output[output_length] = '\0';
                        *next_out = next;
                        return 0;
                }
                if ((label_length & 0xc0U) == 0xc0U) {
                        uint16_t pointer;

                        if (cursor + 1U >= length)
                                return -1;
                        pointer = (uint16_t)(((label_length & 0x3fU) << 8) |
                                             data[cursor + 1U]);
                        if (pointer >= length || ++jumps > length)
                                return -1;
                        if (!jumped) {
                                next = cursor + 2U;
                                jumped = true;
                        }
                        cursor = pointer;
                        continue;
                }
                if ((label_length & 0xc0U) != 0 ||
                    label_length > TG_DNS_LABEL_MAX ||
                    cursor + 1U + label_length > length)
                        return -1;
                if (output_length != 0) {
                        if (output_length + 1U >= output_cap)
                                return -1;
                        output[output_length++] = '.';
                }
                if (output_length + label_length >= output_cap)
                        return -1;
                for (size_t index = 0; index < label_length; index++)
                        output[output_length++] =
                            (char)tg_dns_lower(data[cursor + 1U + index]);
                cursor += 1U + label_length;
        }
}

static bool tg_dns_names_equal(const char *left, const char *right) {
        if (left == NULL || right == NULL)
                return false;
        while (*left != '\0' && *right != '\0') {
                if (tg_dns_lower((unsigned char)*left) !=
                    tg_dns_lower((unsigned char)*right))
                        return false;
                left++;
                right++;
        }
        return *left == '\0' && *right == '\0';
}

/** @brief Validate one complete DNS response datagram. */
static bool tg_dns_response_valid(const struct tg_dns_state *state,
                                  const struct tg_dns_config *config,
                                  const uint8_t *data, size_t length) {
        char expanded_name[TG_DNS_QNAME_CAP];
        size_t offset = 12;
        uint16_t flags;
        uint16_t answer_count;
        uint16_t authority_count;
        uint16_t additional_count;
        size_t section_count;

        if (state == NULL || config == NULL || data == NULL || length < 12 ||
            !tg_dns_qtype_supported(config->qtype))
                return false;
        if (tg_dns_get_u16(data) != state->transaction_id)
                return false;
        flags = tg_dns_get_u16(data + 2);
        if ((flags & UINT16_C(0x8000)) == 0 || /* QR */
            (flags & UINT16_C(0x7800)) != 0 || /* OPCODE */
            (flags & UINT16_C(0x0200)) != 0 || /* TC */
            (flags & UINT16_C(0x000f)) != 0)   /* RCODE */
                return false;
        if (tg_dns_get_u16(data + 4) != 1)
                return false;
        answer_count = tg_dns_get_u16(data + 6);
        authority_count = tg_dns_get_u16(data + 8);
        additional_count = tg_dns_get_u16(data + 10);
        if (answer_count == 0)
                return false;

        if (tg_dns_expand_name(data, length, offset, expanded_name,
                               sizeof(expanded_name), &offset) != 0 ||
            !tg_dns_names_equal(expanded_name, config->qname) ||
            offset + 4U > length ||
            tg_dns_get_u16(data + offset) != config->qtype ||
            tg_dns_get_u16(data + offset + 2U) != 1)
                return false;
        offset += 4U;

        section_count =
            (size_t)answer_count + authority_count + additional_count;
        for (size_t section = 0; section < section_count; section++) {
                uint16_t rdata_length;

                if (tg_dns_expand_name(data, length, offset, expanded_name,
                                       sizeof(expanded_name), &offset) != 0 ||
                    offset + 10U > length)
                        return false;
                rdata_length = tg_dns_get_u16(data + offset + 8U);
                offset += 10U;
                if ((size_t)rdata_length > length - offset)
                        return false;
                offset += rdata_length;
        }
        return offset == length;
}

/** @brief Allocates per-transaction DNS response state. */
static int tg_dns_init(struct tg_txn *txn) {
        const struct tg_dns_config *config;
        struct tg_dns_state *state;

        if (txn == NULL) {
                errno = EINVAL;
                return -1;
        }
        state = calloc(1, sizeof(*state));
        if (state == NULL)
                return -1;
        config = txn->class_config;
        state->transaction_id =
            config == NULL ? TG_DNS_TRANSACTION_ID : config->transaction_id;
        txn->proto_ctx = state;
        return 0;
}

/** @brief DNS has no transmit-side parser state. */
static void tg_dns_on_tx_accepted(__attribute__((unused)) struct tg_txn *txn,
                                  __attribute__((unused)) size_t bytes) {}

/** @brief Parse one complete DNS response datagram. */
static enum tg_proto_result tg_dns_on_rx(struct tg_txn *txn,
                                         const uint8_t *data, size_t len) {
        const struct tg_dns_config *config;
        const struct tg_dns_state *state;

        if (txn == NULL || (data == NULL && len != 0))
                return TG_PROTO_FAILED;
        config = txn->class_config;
        state = txn->proto_ctx;
        return tg_dns_response_valid(state, config, data, len)
                   ? TG_PROTO_COMPLETE
                   : TG_PROTO_FAILED;
}

/** @brief UDP DNS transactions cannot be completed by stream EOF. */
static enum tg_proto_result
tg_dns_on_eof(__attribute__((unused)) struct tg_txn *txn) {
        return TG_PROTO_FAILED;
}

/** @brief Release per-transaction DNS response state. */
static void tg_dns_reset(struct tg_txn *txn) {
        if (txn == NULL)
                return;
        free(txn->proto_ctx);
        txn->proto_ctx = NULL;
}

/** @brief DNS implementation of the generic protocol plugin contract. */
const struct tg_proto_ops tg_dns_proto_ops = {
    .name = "dns",
    .config_clone = tg_dns_config_clone,
    .config_free = tg_dns_config_free,
    .init = tg_dns_init,
    .build_request = tg_dns_build_request,
    .on_tx_accepted = tg_dns_on_tx_accepted,
    .on_rx = tg_dns_on_rx,
    .on_eof = tg_dns_on_eof,
    .reset = tg_dns_reset,
};
