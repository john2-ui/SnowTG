#ifndef TRAFFIC_GEN_TXN_H
#define TRAFFIC_GEN_TXN_H

/**
 * @file txn.h
 * @brief One owner-local L7 request/response transaction.
 */

#include "../proto/proto.h"

#include <stddef.h>
#include <stdint.h>

#define TG_TXN_REQUEST_CAP 1024U

struct tg_txn {
        const struct tg_proto_ops *proto;
        const void *class_config;
        void *proto_ctx;

        uint8_t request[TG_TXN_REQUEST_CAP];
        size_t request_len;
        size_t request_offset;
        uint64_t response_bytes;
};

int tg_txn_init(struct tg_txn *txn, const struct tg_proto_ops *proto,
                const void *class_config);
void tg_txn_reset(struct tg_txn *txn);
void tg_txn_on_tx_accepted(struct tg_txn *txn, size_t bytes);
enum tg_proto_result tg_txn_on_rx(struct tg_txn *txn, const uint8_t *data,
                                  size_t len);
enum tg_proto_result tg_txn_on_eof(struct tg_txn *txn);

#endif /* TRAFFIC_GEN_TXN_H */
