#ifndef TRAFFIC_GEN_TXN_H
#define TRAFFIC_GEN_TXN_H

/**
 * @file txn.h
 * @brief One owner-local L7 request/response transaction and lifecycle API.
 *
 * A transaction owns protocol-parser state and a bounded request buffer.
 * Transport code reports accepted transmit bytes and received data through
 * this interface without knowing protocol-specific parsing details.
 */

#include "../proto/proto.h"

#include <stddef.h>
#include <stdint.h>

/** @brief Maximum serialized application request size, in bytes. */
#define TG_TXN_REQUEST_CAP 1024U

/**
 * @brief Per-flow state for a single application-layer exchange.
 *
 * @p class_config is immutable compiled-plan data.  @p proto_ctx is private
 * plugin state and is released through the protocol reset callback.
 */
struct tg_txn {
        const struct tg_proto_ops *proto;
        const void *class_config;
        void *proto_ctx;

        uint8_t request[TG_TXN_REQUEST_CAP];
        size_t request_len;
        size_t request_offset;
        uint64_t response_bytes;
};

/**
 * @brief Initializes protocol state and serializes the outbound request.
 * @param txn Destination transaction.
 * @param proto Protocol plugin that owns parsing behavior.
 * @param class_config Immutable plugin-specific class configuration.
 * @return 0 on success; -1 with @c errno set on validation or plugin failure.
 */
int tg_txn_init(struct tg_txn *txn, const struct tg_proto_ops *proto,
                const void *class_config);

/**
 * @brief Releases plugin state and clears transaction fields.
 * @param txn Transaction to reset; @c NULL is accepted.
 */
void tg_txn_reset(struct tg_txn *txn);

/**
 * @brief Notifies the protocol that transport accepted request bytes.
 * @param txn Active transaction.
 * @param bytes Number of newly accepted request bytes.
 */
void tg_txn_on_tx_accepted(struct tg_txn *txn, size_t bytes);

/**
 * @brief Delivers an arbitrary received byte chunk to the protocol plugin.
 * @param txn Active transaction.
 * @param data Received bytes, or @c NULL only when @p len is zero.
 * @param len Number of bytes in @p data.
 * @return Protocol progress, completion, or failure result.
 */
enum tg_proto_result tg_txn_on_rx(struct tg_txn *txn, const uint8_t *data,
                                  size_t len);

/**
 * @brief Requests final protocol validation after transport EOF.
 * @param txn Active transaction.
 * @return @ref TG_PROTO_COMPLETE only when EOF legally completes the message.
 */
enum tg_proto_result tg_txn_on_eof(struct tg_txn *txn);

#endif /* TRAFFIC_GEN_TXN_H */
