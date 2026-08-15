#ifndef NETARCH_TCP_MEMORY_H
#define NETARCH_TCP_MEMORY_H

/**
 * @file tcp_memory.h
 * @brief Owner-local fixed-object memory for TCP hot-path state.
 *
 * A TCP owner creates one instance at startup.  Its objects never cross the
 * owner boundary, so DPDK's per-lcore mempool cache remains effective and a
 * local shortage can be reported as backpressure instead of becoming an
 * unbounded rte_malloc allocation.
 */

#include <stdint.h>

struct rte_mempool;
struct tcp_fragment;
struct tcp_ofo_seg;
struct tcp_rx_blob;
struct tcp_sack_range;
struct tcp_tx_chunk;

/** Kinds of fixed objects tracked independently by one TCP owner. */
enum tcp_memory_kind {
        TCP_MEMORY_TX_CHUNK = 0,
        TCP_MEMORY_RX_BLOB,
        TCP_MEMORY_OFO_SEG,
        TCP_MEMORY_FRAGMENT,
        TCP_MEMORY_SACK_RANGE,
        TCP_MEMORY_PAYLOAD,
        TCP_MEMORY_KIND_MAX,
};

/** Per-owner pool handles and owner-local allocation accounting. */
struct tcp_owner_memory {
        struct rte_mempool *pools[TCP_MEMORY_KIND_MAX]; /**< Pool by kind. */
        uint32_t capacity[TCP_MEMORY_KIND_MAX];   /**< Fixed pool capacities. */
        uint32_t alloc_fail[TCP_MEMORY_KIND_MAX]; /**< Failed get counts. */
        uint32_t
            peak_in_use[TCP_MEMORY_KIND_MAX]; /**< Peak checked-out count. */
        uint16_t lcore_id; /**< Sole lcore allowed to use these pools. */
};

/** Read-only point-in-time view consumed by a co-located scheduler. */
struct tcp_memory_snapshot {
        uint32_t
            capacity[TCP_MEMORY_KIND_MAX]; /**< Configured object budget. */
        uint32_t available[TCP_MEMORY_KIND_MAX]; /**< Currently free objects. */
        uint32_t
            alloc_fail[TCP_MEMORY_KIND_MAX]; /**< Cumulative get failures. */
        uint32_t peak_in_use[TCP_MEMORY_KIND_MAX]; /**< High-water usage. */
};

/**
 * Create every fixed-object TCP pool for one owner lcore.
 * @return 0 on success, or -1 after releasing any partially created pools.
 */
int tcp_owner_memory_init(struct tcp_owner_memory *memory,
                          unsigned int lcore_id);
/** Destroy every pool belonging to an owner after all its TCBs are drained. */
void tcp_owner_memory_fini(struct tcp_owner_memory *memory);
/** Copy current pool availability and cumulative allocation failures. */
void tcp_owner_memory_snapshot(const struct tcp_owner_memory *memory,
                               struct tcp_memory_snapshot *snapshot);
/** Return non-zero when any owned pool has crossed its admission low water. */
int tcp_owner_memory_below_low_water(const struct tcp_owner_memory *memory);
/** Return non-zero only when every owned pool has crossed its resume water. */
int tcp_owner_memory_above_high_water(const struct tcp_owner_memory *memory);

/** Acquire a zeroed TX chunk descriptor from its owning memory domain. */
struct tcp_tx_chunk *tcp_memory_tx_chunk_alloc(struct tcp_owner_memory *memory);
/** Return a TX chunk descriptor after its payload has been released. */
void tcp_memory_tx_chunk_free(struct tcp_owner_memory *memory,
                              struct tcp_tx_chunk *chunk);
/** Acquire a zeroed queued-RX descriptor from its owning memory domain. */
struct tcp_rx_blob *tcp_memory_rx_blob_alloc(struct tcp_owner_memory *memory);
/** Return a queued-RX descriptor after its payload has been released. */
void tcp_memory_rx_blob_free(struct tcp_owner_memory *memory,
                             struct tcp_rx_blob *blob);
/** Acquire a zeroed out-of-order segment descriptor from its owner. */
struct tcp_ofo_seg *tcp_memory_ofo_seg_alloc(struct tcp_owner_memory *memory);
/** Return an out-of-order descriptor after its payload has been released. */
void tcp_memory_ofo_seg_free(struct tcp_owner_memory *memory,
                             struct tcp_ofo_seg *segment);
/** Acquire a zeroed queued TCP control-fragment descriptor from its owner. */
struct tcp_fragment *tcp_memory_fragment_alloc(struct tcp_owner_memory *memory);
/** Return a queued TCP control-fragment descriptor to its owner. */
void tcp_memory_fragment_free(struct tcp_owner_memory *memory,
                              struct tcp_fragment *fragment);
/** Acquire one sender SACK interval node from its owner-local pool. */
struct tcp_sack_range *
tcp_memory_sack_range_alloc(struct tcp_owner_memory *memory);
/** Return one sender SACK interval node to its owner-local pool. */
void tcp_memory_sack_range_free(struct tcp_owner_memory *memory,
                                struct tcp_sack_range *range);
/**
 * Acquire one fixed-size payload block and expose its byte storage.
 * @p storage is an opaque pool object that must later be returned unchanged.
 */
int tcp_memory_payload_alloc(struct tcp_owner_memory *memory, uint8_t **data,
                             void **storage);
/** Return the opaque backing object received from @ref
 * tcp_memory_payload_alloc. */
void tcp_memory_payload_free(struct tcp_owner_memory *memory, void *storage);

#endif /* NETARCH_TCP_MEMORY_H */
