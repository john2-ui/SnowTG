#ifndef TRAFFIC_GEN_PROTO_H
#define TRAFFIC_GEN_PROTO_H

/**
 * @file proto.h
 * @brief L7 protocol operations for owner-local traffic-generator transactions.
 */

#include <stddef.h>
#include <stdint.h>

struct tg_txn;

enum tg_proto_result {
        TG_PROTO_MORE = 0,
        TG_PROTO_COMPLETE,
        TG_PROTO_FAILED,
};

/**
 * Protocol plugins only transform and judge transaction bytes.  Transport I/O
 * remains in core/flow.c so plugins never access owner_io_* APIs directly.
 */
struct tg_proto_ops {
        const char *name;

        int (*build_request)(const void *class_config, uint8_t *buffer,
                             size_t buffer_cap, size_t *request_len_out);
        void (*on_tx_accepted)(struct tg_txn *txn, size_t bytes);
        enum tg_proto_result (*on_rx)(struct tg_txn *txn, const uint8_t *data,
                                      size_t len);
        enum tg_proto_result (*on_eof)(struct tg_txn *txn);
        void (*reset)(struct tg_txn *txn);
};

#endif /* TRAFFIC_GEN_PROTO_H */
