#include "tcp_memory.h"

#include "config.h"
#include "log.h"
#include "tcp.h"

#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_mempool.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/** One pool object that owns exactly one fixed-size TCP payload region. */
struct tcp_payload_block {
        uint8_t bytes[TCP_MEMORY_CHUNK_SIZE];
};

/** Stable mempool name stems; the owner lcore suffix makes them unique. */
static const char *const tcp_memory_names[TCP_MEMORY_KIND_MAX] = {
    "tcp_tx_chunk", "tcp_rx_blob", "tcp_ofo_seg", "tcp_fragment", "tcp_payload",
};

/** Per-kind object budgets configured for one owner worker. */
static const uint32_t tcp_memory_counts[TCP_MEMORY_KIND_MAX] = {
    TCP_MEMORY_TX_CHUNKS, TCP_MEMORY_RX_BLOBS,       TCP_MEMORY_OFO_SEGS,
    TCP_MEMORY_FRAGMENTS, TCP_MEMORY_PAYLOAD_BLOCKS,
};

/** Concrete allocation sizes associated with each @ref tcp_memory_kind. */
static const unsigned int tcp_memory_sizes[TCP_MEMORY_KIND_MAX] = {
    sizeof(struct tcp_tx_chunk),      sizeof(struct tcp_rx_blob),
    sizeof(struct tcp_ofo_seg),       sizeof(struct tcp_fragment),
    sizeof(struct tcp_payload_block),
};

/** Update one pool's high-water mark immediately after a successful get. */
static void tcp_memory_update_peak(struct tcp_owner_memory *memory,
                                   enum tcp_memory_kind kind) {
        uint32_t available;
        uint32_t used;

        available = rte_mempool_avail_count(memory->pools[kind]);
        used = memory->capacity[kind] - available;
        if (used > memory->peak_in_use[kind])
                memory->peak_in_use[kind] = used;
}

/**
 * Get and zero one object while recording allocation failures and peak usage.
 * The caller is already on @p memory's owner lcore.
 */
static void *tcp_memory_get(struct tcp_owner_memory *memory,
                            enum tcp_memory_kind kind) {
        void *object;

        if (memory == NULL || memory->pools[kind] == NULL ||
            rte_mempool_get(memory->pools[kind], &object) != 0) {
                if (memory != NULL)
                        memory->alloc_fail[kind]++;
                errno = ENOBUFS;
                return NULL;
        }
        memset(object, 0, tcp_memory_sizes[kind]);
        tcp_memory_update_peak(memory, kind);
        return object;
}

/** Return an object to the precise pool from which the owner acquired it. */
static void tcp_memory_put(struct tcp_owner_memory *memory,
                           enum tcp_memory_kind kind, void *object) {
        if (memory != NULL && object != NULL && memory->pools[kind] != NULL)
                rte_mempool_put(memory->pools[kind], object);
}

/** @copydoc tcp_owner_memory_init */
int tcp_owner_memory_init(struct tcp_owner_memory *memory,
                          unsigned int lcore_id) {
        if (memory == NULL)
                return -1;

        memset(memory, 0, sizeof(*memory));
        memory->lcore_id = (uint16_t)lcore_id;
        for (unsigned int kind = 0; kind < TCP_MEMORY_KIND_MAX; kind++) {
                char name[RTE_MEMPOOL_NAMESIZE];

                (void)snprintf(name, sizeof(name), "%s_%u",
                               tcp_memory_names[kind], lcore_id);
                memory->pools[kind] = rte_mempool_create(
                    name, tcp_memory_counts[kind], tcp_memory_sizes[kind], 0, 0,
                    NULL, NULL, NULL, NULL, rte_socket_id(), 0);
                if (memory->pools[kind] == NULL) {
                        LOG_ERROR(
                            "tcp memory pool init failed kind=%s count=%u "
                            "size=%u errno=%d (%s)",
                            tcp_memory_names[kind], tcp_memory_counts[kind],
                            tcp_memory_sizes[kind], rte_errno,
                            rte_strerror(rte_errno));
                        tcp_owner_memory_fini(memory);
                        return -1;
                }
                memory->capacity[kind] = tcp_memory_counts[kind];
        }
        return 0;
}

/** @copydoc tcp_owner_memory_fini */
void tcp_owner_memory_fini(struct tcp_owner_memory *memory) {
        if (memory == NULL)
                return;
        for (unsigned int kind = 0; kind < TCP_MEMORY_KIND_MAX; kind++) {
                if (memory->pools[kind] != NULL)
                        rte_mempool_free(memory->pools[kind]);
        }
        memset(memory, 0, sizeof(*memory));
}

/** @copydoc tcp_owner_memory_snapshot */
void tcp_owner_memory_snapshot(const struct tcp_owner_memory *memory,
                               struct tcp_memory_snapshot *snapshot) {
        if (snapshot == NULL)
                return;
        memset(snapshot, 0, sizeof(*snapshot));
        if (memory == NULL)
                return;
        for (unsigned int kind = 0; kind < TCP_MEMORY_KIND_MAX; kind++) {
                snapshot->capacity[kind] = memory->capacity[kind];
                snapshot->available[kind] =
                    memory->pools[kind] == NULL
                        ? 0
                        : rte_mempool_avail_count(memory->pools[kind]);
                snapshot->alloc_fail[kind] = memory->alloc_fail[kind];
                snapshot->peak_in_use[kind] = memory->peak_in_use[kind];
        }
}

/** @copydoc tcp_owner_memory_below_low_water */
int tcp_owner_memory_below_low_water(const struct tcp_owner_memory *memory) {
        if (memory == NULL)
                return 1;
        for (unsigned int kind = 0; kind < TCP_MEMORY_KIND_MAX; kind++) {
                if (memory->pools[kind] != NULL &&
                    rte_mempool_avail_count(memory->pools[kind]) <
                        TCP_MEMORY_LOW_WATER)
                        return 1;
        }
        return 0;
}

/** @copydoc tcp_owner_memory_above_high_water */
int tcp_owner_memory_above_high_water(const struct tcp_owner_memory *memory) {
        if (memory == NULL)
                return 0;
        for (unsigned int kind = 0; kind < TCP_MEMORY_KIND_MAX; kind++) {
                if (memory->pools[kind] != NULL &&
                    rte_mempool_avail_count(memory->pools[kind]) <=
                        TCP_MEMORY_HIGH_WATER)
                        return 0;
        }
        return 1;
}

/** @copydoc tcp_memory_tx_chunk_alloc */
struct tcp_tx_chunk *
tcp_memory_tx_chunk_alloc(struct tcp_owner_memory *memory) {
        return tcp_memory_get(memory, TCP_MEMORY_TX_CHUNK);
}

/** @copydoc tcp_memory_tx_chunk_free */
void tcp_memory_tx_chunk_free(struct tcp_owner_memory *memory,
                              struct tcp_tx_chunk *chunk) {
        tcp_memory_put(memory, TCP_MEMORY_TX_CHUNK, chunk);
}

/** @copydoc tcp_memory_rx_blob_alloc */
struct tcp_rx_blob *tcp_memory_rx_blob_alloc(struct tcp_owner_memory *memory) {
        return tcp_memory_get(memory, TCP_MEMORY_RX_BLOB);
}

/** @copydoc tcp_memory_rx_blob_free */
void tcp_memory_rx_blob_free(struct tcp_owner_memory *memory,
                             struct tcp_rx_blob *blob) {
        tcp_memory_put(memory, TCP_MEMORY_RX_BLOB, blob);
}

/** @copydoc tcp_memory_ofo_seg_alloc */
struct tcp_ofo_seg *tcp_memory_ofo_seg_alloc(struct tcp_owner_memory *memory) {
        return tcp_memory_get(memory, TCP_MEMORY_OFO_SEG);
}

/** @copydoc tcp_memory_ofo_seg_free */
void tcp_memory_ofo_seg_free(struct tcp_owner_memory *memory,
                             struct tcp_ofo_seg *segment) {
        tcp_memory_put(memory, TCP_MEMORY_OFO_SEG, segment);
}

/** @copydoc tcp_memory_fragment_alloc */
struct tcp_fragment *
tcp_memory_fragment_alloc(struct tcp_owner_memory *memory) {
        return tcp_memory_get(memory, TCP_MEMORY_FRAGMENT);
}

/** @copydoc tcp_memory_fragment_free */
void tcp_memory_fragment_free(struct tcp_owner_memory *memory,
                              struct tcp_fragment *fragment) {
        tcp_memory_put(memory, TCP_MEMORY_FRAGMENT, fragment);
}

/** @copydoc tcp_memory_payload_alloc */
int tcp_memory_payload_alloc(struct tcp_owner_memory *memory, uint8_t **data,
                             void **storage) {
        struct tcp_payload_block *block;

        if (data == NULL || storage == NULL)
                return -1;
        block = tcp_memory_get(memory, TCP_MEMORY_PAYLOAD);
        if (block == NULL)
                return -1;
        *data = block->bytes;
        *storage = block;
        return 0;
}

/** @copydoc tcp_memory_payload_free */
void tcp_memory_payload_free(struct tcp_owner_memory *memory, void *storage) {
        tcp_memory_put(memory, TCP_MEMORY_PAYLOAD, storage);
}
