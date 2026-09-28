#ifndef NETARCH_RESOURCE_H
#define NETARCH_RESOURCE_H

#include <stdint.h>

/* Owner-written lifetime counters. Zero capacity means no independent budget
 * for queue gauges; pool capacities are always the actual configured budget.
 * Failure reasons are per operation/resource, never additive request counts. */
#define RESOURCE_FIELDS(X) \
        X(capacity) X(current) X(peak) X(exhausted) X(unavailable) X(busy) X(limit)
struct resource_metric {
#define RESOURCE_FIELD(name) uint64_t name;
        RESOURCE_FIELDS(RESOURCE_FIELD)
#undef RESOURCE_FIELD
};

static inline void resource_acquire(struct resource_metric *r, uint64_t n) {
        r->current += n;
        if (r->current > r->peak)
                r->peak = r->current;
}
static inline void resource_release(struct resource_metric *r, uint64_t n) {
        r->current -= n;
}

/* Stable exported names; append entries when extending the resource schema. */
#define OWNER_RESOURCE_NAMES(X) \
        X(tcp_tx_chunk) X(tcp_rx_blob) X(tcp_ofo_seg) X(tcp_fragment) \
        X(tcp_sack_range) X(tcp_payload) X(udp_rx_node) X(socket_slot) \
        X(ready_event) X(timer) X(time_wait) X(tcp_sndbuf_bytes) \
        X(tcp_unacked_bytes) X(ofo_segments) X(ofo_bytes)
enum owner_resource_kind {
#define RESOURCE_KIND(name) OWNER_RESOURCE_##name,
        OWNER_RESOURCE_NAMES(RESOURCE_KIND)
#undef RESOURCE_KIND
        OWNER_RESOURCE_COUNT
};
struct owner_resource_snapshot {
        struct resource_metric values[OWNER_RESOURCE_COUNT];
};
#endif
