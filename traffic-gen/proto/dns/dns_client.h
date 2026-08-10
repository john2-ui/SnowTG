#ifndef TRAFFIC_GEN_DNS_CLIENT_H
#define TRAFFIC_GEN_DNS_CLIENT_H

/**
 * @file dns_client.h
 * @brief DNS-over-UDP client protocol configuration and plugin exports.
 *
 * The first DNS client subset sends one IN question for an A or AAAA record
 * and accepts one complete response datagram.  It owns no socket state.
 */

#include "../proto.h"

#include <stdint.h>

/** Capacity, including NUL, of a textual DNS name in a scenario. */
#define TG_DNS_QNAME_CAP 256U
/** Maximum wire-format DNS label length. */
#define TG_DNS_LABEL_MAX 63U
/** Fixed query ID used by the immutable request-template model. */
#define TG_DNS_TRANSACTION_ID UINT16_C(1)

/** DNS record types supported by the first client subset. */
enum tg_dns_qtype {
        TG_DNS_QTYPE_A = 1,
        TG_DNS_QTYPE_AAAA = 28,
};

/** Immutable owning configuration for one DNS traffic class. */
struct tg_dns_config {
        char qname[TG_DNS_QNAME_CAP];
        uint16_t qtype;
        uint16_t transaction_id;
};

/** DNS protocol operations used by scenario-compiled DNS classes. */
extern const struct tg_proto_ops tg_dns_proto_ops;

/**
 * Validate a presentation-form qname used by scenarios and request builders.
 *
 * @return 0 on success; -1 with @c errno set to @c EINVAL on invalid syntax.
 */
int tg_dns_qname_validate(const char *qname);

#endif /* TRAFFIC_GEN_DNS_CLIENT_H */
