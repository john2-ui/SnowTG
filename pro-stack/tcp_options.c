/**
 * @file tcp_options.c
 * @brief TCP option wire encoding, parsing, and per-stream negotiation.
 */
#include "tcp_options.h"

#include "config.h"
#include "socket.h"

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_memcpy.h>

#include <string.h>

#define TCP_OPT_EOL 0
#define TCP_OPT_NOP 1
#define TCP_OPT_MSS 2
#define TCP_OPT_WSCALE 3
#define TCP_OPT_TIMESTAMP 8

#define TCP_OPT_MSS_LEN 4
#define TCP_OPT_WSCALE_LEN 3
#define TCP_OPT_TIMESTAMP_LEN 10
#define TCP_OPT_TIMESTAMP_PADDED_LEN 12

#define TCP_MIN_MSS 536

static uint32_t tcp_options_ts_now(struct nsock *sk) {
        uint64_t cycles = rte_get_timer_cycles();
        uint32_t now = (uint32_t)(cycles * 1000 / rte_get_timer_hz());

        sk->u.tcp.ts_last_val = now;
        return now;
}

static int tcp_options_append(struct tcp_fragment *f, const void *data,
                              size_t len) {
        size_t offset = (size_t)f->opt_len * sizeof(uint32_t);

        if (offset + len > sizeof(f->options))
                return -1;

        rte_memcpy((uint8_t *)f->options + offset, data, len);
        f->opt_len =
            (int)((offset + len + sizeof(uint32_t) - 1) / sizeof(uint32_t));
        return 0;
}

static int tcp_options_emit_mss(struct tcp_fragment *f, uint16_t mss) {
        const uint8_t opt[TCP_OPT_MSS_LEN] = {
            TCP_OPT_MSS,
            TCP_OPT_MSS_LEN,
            (uint8_t)(mss >> 8),
            (uint8_t)mss,
        };

        return tcp_options_append(f, opt, sizeof(opt));
}

static int tcp_options_emit_wscale(struct tcp_fragment *f, uint8_t wscale) {
        const uint8_t opt[4] = {
            TCP_OPT_NOP,
            TCP_OPT_WSCALE,
            TCP_OPT_WSCALE_LEN,
            wscale,
        };

        return tcp_options_append(f, opt, sizeof(opt));
}

static int tcp_options_emit_timestamp(struct tcp_fragment *f, uint32_t tsval,
                                      uint32_t tsecr) {
        uint8_t opt[TCP_OPT_TIMESTAMP_PADDED_LEN] = {
            TCP_OPT_NOP,
            TCP_OPT_NOP,
            TCP_OPT_TIMESTAMP,
            TCP_OPT_TIMESTAMP_LEN,
        };
        uint32_t value;

        value = rte_cpu_to_be_32(tsval);
        rte_memcpy(opt + 4, &value, sizeof(value));
        value = rte_cpu_to_be_32(tsecr);
        rte_memcpy(opt + 8, &value, sizeof(value));
        return tcp_options_append(f, opt, sizeof(opt));
}

static void tcp_options_finish(struct tcp_fragment *f) {
        f->data_off = (uint8_t)((5 + f->opt_len) << 4);
}

void tcp_options_reset_state(struct nsock *sk) {
        sk->u.tcp.timestamps_ok = false;
        sk->u.tcp.ts_recent = 0;
        sk->u.tcp.ts_last_val = 0;
}

int tcp_options_parse(const struct rte_tcp_hdr *hdr,
                      struct tcp_options_rx *rx) {
        const uint8_t hdr_len = (hdr->data_off >> 4) * 4;
        const uint8_t *opt;
        size_t remain;
        bool mss_present = false;

        if (hdr_len < sizeof(*hdr))
                return -1;

        memset(rx, 0, sizeof(*rx));
        rx->mss = TCP_DEFAULT_MSS;
        opt = (const uint8_t *)hdr + sizeof(*hdr);
        remain = hdr_len - sizeof(*hdr);

        while (remain > 0) {
                uint8_t kind = opt[0];
                uint8_t len;

                if (kind == TCP_OPT_EOL)
                        break;
                if (kind == TCP_OPT_NOP) {
                        opt++;
                        remain--;
                        continue;
                }
                if (remain < 2)
                        return -1;

                len = opt[1];
                if (len < 2 || len > remain)
                        return -1;

                switch (kind) {
                case TCP_OPT_MSS:
                        if (len != TCP_OPT_MSS_LEN || mss_present)
                                return -1;
                        mss_present = true;
                        {
                                uint16_t value =
                                    ((uint16_t)opt[2] << 8) | opt[3];
                                if (value >= TCP_MIN_MSS)
                                        rx->mss = value;
                        }
                        break;
                case TCP_OPT_WSCALE:
                        if (len != TCP_OPT_WSCALE_LEN || rx->wscale_present ||
                            opt[2] > TCP_WSCALE_MAX)
                                return -1;
                        rx->wscale = opt[2];
                        rx->wscale_present = true;
                        break;
                case TCP_OPT_TIMESTAMP:
                        if (len != TCP_OPT_TIMESTAMP_LEN ||
                            rx->timestamp_present)
                                return -1;
                        {
                                uint32_t tsval;
                                uint32_t tsecr;

                                rte_memcpy(&tsval, opt + 2, sizeof(tsval));
                                rte_memcpy(&tsecr, opt + 6, sizeof(tsecr));
                                rx->tsval = rte_be_to_cpu_32(tsval);
                                rx->tsecr = rte_be_to_cpu_32(tsecr);
                                rx->timestamp_present = true;
                        }
                        break;
                default:
                        /*
                         * TODO(SACK): parse kind 4 on SYN and kind 5 on ACKs
                         * once sender-side selective retransmission exists.
                         */
                        break;
                }

                opt += len;
                remain -= len;
        }
        return 0;
}

void tcp_options_negotiate_syn(struct nsock *sk,
                               const struct tcp_options_rx *peer) {
        sk->u.tcp.snd_mss =
            peer->mss < TCP_DEFAULT_MSS ? peer->mss : TCP_DEFAULT_MSS;
        sk->u.tcp.snd_wscale = peer->wscale;
        sk->u.tcp.wscale_ok = peer->wscale_present;
        sk->u.tcp.timestamps_ok = peer->timestamp_present;
        if (peer->timestamp_present)
                sk->u.tcp.ts_recent = peer->tsval;
}

int tcp_options_apply_syn(struct nsock *sk, struct tcp_fragment *f,
                          bool include_wscale, bool include_timestamp,
                          uint32_t tsecr) {
        memset(f->options, 0, sizeof(f->options));
        f->opt_len = 0;

        if (tcp_options_emit_mss(f, TCP_DEFAULT_MSS) != 0 ||
            (include_wscale &&
             tcp_options_emit_wscale(f, sk->u.tcp.rcv_wscale) != 0) ||
            (include_timestamp &&
             tcp_options_emit_timestamp(f, tcp_options_ts_now(sk), tsecr) != 0))
                return -1;

        tcp_options_finish(f);
        return 0;
}

int tcp_options_apply_established(struct nsock *sk, struct tcp_fragment *f) {
        memset(f->options, 0, sizeof(f->options));
        f->opt_len = 0;

        if (sk->u.tcp.timestamps_ok &&
            tcp_options_emit_timestamp(f, tcp_options_ts_now(sk),
                                       sk->u.tcp.ts_recent) != 0)
                return -1;

        tcp_options_finish(f);
        return 0;
}

int tcp_options_process_inbound(struct nsock *sk,
                                const struct tcp_options_rx *rx, bool is_rst) {
        if (is_rst || !sk->u.tcp.timestamps_ok)
                return 0;
        if (!rx->timestamp_present)
                return -1;

        /*
         * TODO(PAWS): apply the RFC 7323 serial timestamp comparison only
         * after sequence acceptability is centralized for every TCP state.
         */
        sk->u.tcp.ts_recent = rx->tsval;

        /*
         * TODO(RTT/RTO): use rx->tsecr to identify a non-retransmitted
         * measurement probe and update the existing fixed data RTO.
         */
        return 0;
}

uint16_t tcp_options_data_mss(const struct nsock *sk) {
        uint16_t peer_mss = sk->u.tcp.snd_mss;
        uint16_t local_mss = TCP_DEFAULT_MSS;

        if (peer_mss == 0)
                peer_mss = TCP_DEFAULT_MSS;
        /* Timestamp option is 12 bytes, so we need to subtract it from the
         * local MSS */
        if (sk->u.tcp.timestamps_ok)
                local_mss -= TCP_OPT_TIMESTAMP_PADDED_LEN;
        return peer_mss < local_mss ? peer_mss : local_mss;
}
