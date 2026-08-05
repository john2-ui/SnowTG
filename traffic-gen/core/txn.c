/**
 * @file txn.c
 * @brief Implements protocol-neutral transaction initialization and dispatch.
 *
 * This module is the boundary between nonblocking transport and byte-only
 * protocol plugins.  It owns no socket state and relies on plugin callbacks
 * to allocate, consume, and release protocol-specific parser context.
 */

#include "txn.h"

#include <errno.h>
#include <string.h>

/** @copydoc tg_txn_init */
int tg_txn_init(struct tg_txn *txn, const struct tg_proto_ops *proto,
                const void *class_config) {
        size_t request_len;

        if (txn == NULL || proto == NULL || proto->build_request == NULL) {
                errno = EINVAL;
                return -1;
        }

        memset(txn, 0, sizeof(*txn));
        txn->proto = proto;
        txn->class_config = class_config;

        if (proto->init != NULL && proto->init(txn) != 0) {
                tg_txn_reset(txn);
                return -1;
        }

        if (proto->build_request(class_config, txn->request,
                                 sizeof(txn->request), &request_len) != 0 ||
            request_len == 0 || request_len > sizeof(txn->request)) {
                if (errno == 0)
                        errno = EINVAL;
                tg_txn_reset(txn);
                return -1;
        }

        txn->request_len = request_len;
        return 0;
}

/** @copydoc tg_txn_reset */
void tg_txn_reset(struct tg_txn *txn) {
        if (txn == NULL)
                return;

        if (txn->proto != NULL && txn->proto->reset != NULL)
                txn->proto->reset(txn);
        memset(txn, 0, sizeof(*txn));
}

/** @copydoc tg_txn_on_tx_accepted */
void tg_txn_on_tx_accepted(struct tg_txn *txn, size_t bytes) {
        if (txn != NULL && txn->proto != NULL &&
            txn->proto->on_tx_accepted != NULL)
                txn->proto->on_tx_accepted(txn, bytes);
}

/** @copydoc tg_txn_on_rx */
enum tg_proto_result tg_txn_on_rx(struct tg_txn *txn, const uint8_t *data,
                                  size_t len) {
        if (txn == NULL || txn->proto == NULL || txn->proto->on_rx == NULL)
                return TG_PROTO_FAILED;

        txn->response_bytes += len;
        return txn->proto->on_rx(txn, data, len);
}

/** @copydoc tg_txn_on_eof */
enum tg_proto_result tg_txn_on_eof(struct tg_txn *txn) {
        if (txn == NULL || txn->proto == NULL || txn->proto->on_eof == NULL)
                return TG_PROTO_FAILED;

        return txn->proto->on_eof(txn);
}
