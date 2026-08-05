#ifndef TRAFFIC_GEN_PROTO_H
#define TRAFFIC_GEN_PROTO_H

/**
 * @file proto.h
 * @brief Protocol-plugin contract for owner-local traffic-generator
 * transactions.
 *
 * Plugins construct and interpret application bytes only.  The flow layer
 * performs all nonblocking socket operations and maps plugin outcomes into
 * flow lifecycle results.
 */

#include <stddef.h>
#include <stdint.h>

struct tg_txn;

/** @brief Progress status returned by a protocol plugin callback. */
enum tg_proto_result {
        /** @brief More bytes or EOF processing are required. */
        TG_PROTO_MORE = 0,
        /** @brief Exactly one complete, valid response was observed. */
        TG_PROTO_COMPLETE,
        /** @brief The request or response violates plugin requirements. */
        TG_PROTO_FAILED,
};

/**
 * @brief Vtable defining the byte-oriented lifecycle of one protocol plugin.
 *
 * Protocol plugins never access @c owner_io_* APIs directly.  @p class_config
 * remains owned by the compiled scenario; @p txn provides plugin-private
 * storage through its @c proto_ctx field.
 */
struct tg_proto_ops {
        const char *name;

        int (*init)(struct tg_txn *txn);
        int (*build_request)(const void *class_config, uint8_t *buffer,
                             size_t buffer_cap, size_t *request_len_out);
        void (*on_tx_accepted)(struct tg_txn *txn, size_t bytes);
        enum tg_proto_result (*on_rx)(struct tg_txn *txn, const uint8_t *data,
                                      size_t len);
        enum tg_proto_result (*on_eof)(struct tg_txn *txn);
        void (*reset)(struct tg_txn *txn);
};

#endif /* TRAFFIC_GEN_PROTO_H */
