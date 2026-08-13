#ifndef TRAFFIC_GEN_TXN_H
#define TRAFFIC_GEN_TXN_H

/**
 * @file txn.h
 * @brief One owner-local L7 request/response transaction and lifecycle API.
 *
 * A transaction owns protocol-parser state and references immutable request
 * bytes owned by its compiled scenario.
 * Transport code reports accepted transmit bytes and received data through
 * this interface without knowing protocol-specific parsing details.
 */

#include "../proto/proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

        const uint8_t *request;
        size_t request_len;
        size_t request_offset;
        uint64_t response_bytes;
        /** True when the completed response permits TCP connection reuse. */
        bool connection_reusable;
};

/**
 * @brief Initializes protocol state without allocating request payload.
 * @param txn Destination transaction.
 * @param proto Protocol plugin that owns parsing behavior.
 * @param class_config Immutable plugin-specific class configuration.
 * @return 0 on success; -1 with @c errno set on validation or plugin failure.
 */
int tg_txn_init(struct tg_txn *txn, const struct tg_proto_ops *proto,
                const void *class_config);
/**
 * Initialize with a scenario-owned immutable request view.
 * @p request must outlive the transaction; the transaction never frees or
 * copies it, so many flows can share one compiled class template.
 */
int tg_txn_init_with_request(struct tg_txn *txn,
                             const struct tg_proto_ops *proto,
                             const void *class_config, const uint8_t *request,
                             size_t request_len);

/**
 * @brief Releases plugin state and clears transaction fields.
 * @param txn Transaction to reset; @c NULL is accepted.
 */
void tg_txn_reset(struct tg_txn *txn);

/**
 * @brief Reinitializes one transaction for a new request on the same socket.
 *
 * Existing protocol state is reset before the new parser state is created.
 * The request bytes remain a borrowed view owned by the compiled scenario.
 */
int tg_txn_rearm_with_request(struct tg_txn *txn,
                              const struct tg_proto_ops *proto,
                              const void *class_config,
                              const uint8_t *request, size_t request_len);

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
