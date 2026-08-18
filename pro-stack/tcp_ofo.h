#ifndef NETARCH_TCP_OFO_H
#define NETARCH_TCP_OFO_H

/**
 * @file tcp_ofo.h
 * @brief Internal TCP out-of-order queue and pressure-policy interface.
 */

#include <stdbool.h>
#include <stdint.h>

struct nsock;

/** Callback used when an OFO drain reaches contiguous application data. */
typedef int (*tcp_ofo_deliver_fn)(struct nsock *sk, const uint8_t *data,
                                  uint32_t len);
/** Callback used when a drained FIN establishes the stream EOF boundary. */
typedef void (*tcp_ofo_eof_fn)(struct nsock *sk);

/** Insert payload after trimming acknowledged and already-buffered ranges. */
int tcp_ofo_queue_insert(struct nsock *sk, uint32_t seq, const uint8_t *data,
                         uint32_t len, int has_fin);
/** Deliver and release every contiguous OFO node accepted by @p deliver. */
void tcp_ofo_drain(struct nsock *sk, tcp_ofo_deliver_fn deliver,
                   tcp_ofo_eof_fn eof);
/** Release every OFO node and reset the per-TCB reordering episode. */
void tcp_ofo_purge(struct nsock *sk);
/** Count a receive-window rejection unless the range is already buffered. */
void tcp_ofo_record_rcv_window_drop(struct nsock *sk, uint32_t seq,
                                    uint32_t len, bool has_fin);

#endif /* NETARCH_TCP_OFO_H */
