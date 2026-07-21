/**
 * @file arp.h
 * @brief ARP table plus ARP packet build/handle helpers.
 *
 * The table state lives in arp.c (not in this header) so that including the
 * header from several translation units no longer creates independent copies
 * of the table.
 */
#ifndef NETARCH_ARP_H
#define NETARCH_ARP_H

#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <stdint.h>

/**
 * @brief Insert an item at the head of a doubly linked list.
 *
 * @param item Pointer to the item to insert.
 * @param list Head pointer of the list. It is updated by this macro.
 */
#define LL_ADD(item, list)                                                     \
        do {                                                                   \
                (item)->prev = NULL;                                           \
                (item)->next = (list);                                         \
                if ((list) != NULL)                                            \
                        (list)->prev = (item);                                 \
                (list) = (item);                                               \
        } while (0)

/**
 * @brief Remove an item from a doubly linked list.
 *
 * @param item Pointer to the item to remove.
 * @param list Head pointer of the list. It is updated when removing the head.
 */
#define LL_REMOVE(item, list)                                                  \
        do {                                                                   \
                if ((item)->prev != NULL)                                      \
                        (item)->prev->next = (item)->next;                     \
                if ((item)->next != NULL)                                      \
                        (item)->next->prev = (item)->prev;                     \
                if ((list) == (item))                                          \
                        (list) = (item)->next;                                 \
                (item)->prev = NULL;                                           \
                (item)->next = NULL;                                           \
        } while (0)

/**
 * @brief One IPv4-to-Ethernet-address mapping in the ARP table.
 */
struct arp_entry {
        uint32_t ip; /**< IPv4 address in network byte order. */
        uint8_t hwaddr[RTE_ETHER_ADDR_LEN]; /**< Associated Ethernet address. */
        uint8_t type;                       /**< Entry type or state. */
        struct arp_entry *prev; /**< Previous entry, NULL at list head. */
        struct arp_entry *next; /**< Next entry, NULL at list tail. */
};

/**
 * @brief Container for all ARP entries.
 */
struct arp_table {
        struct arp_entry *entries; /**< Head of the ARP entry list. */
        int count;                 /**< Number of entries in the table. */
};

/**
 * @brief Get the singleton ARP table, creating it on first use.
 * @return Pointer to the initialized ARP table (never NULL).
 */
struct arp_table *arp_table_instance(void);

/**
 * @brief Look up an Ethernet address by IPv4 address.
 *
 * @param ip IPv4 address to find, in network byte order.
 * @return Pointer to the matching entry's Ethernet address, or NULL if absent.
 * @warning The returned pointer is owned by the table; do not free it.
 */
uint8_t *arp_lookup(uint32_t ip);

/**
 * @brief Insert a mapping if the IP is not already known.
 *
 * @param ip  IPv4 address in network byte order.
 * @param mac Ethernet address to associate with @p ip.
 */
void arp_table_add(uint32_t ip, const uint8_t *mac);

/**
 * @brief Build an ARP request/reply packet into a fresh mbuf.
 *
 * @param mp      Mempool used to allocate the mbuf.
 * @param opcode  RTE_ARP_OP_REQUEST or RTE_ARP_OP_REPLY.
 * @param dst_mac Target Ethernet address.
 * @param src_ip  Sender IPv4 (network order).
 * @param dst_ip  Target IPv4 (network order).
 * @return Newly allocated mbuf, or NULL on allocation failure.
 */
struct rte_mbuf *arp_build_pkt(struct rte_mempool *mp, uint16_t opcode,
                               const uint8_t *dst_mac, uint32_t src_ip,
                               uint32_t dst_ip);

/**
 * @brief Handle one inbound ARP frame.
 *
 * Replies to requests addressed to the local IP and learns replies into the
 * table. The inbound mbuf is always freed before returning.
 *
 * @param mp   Mempool for any reply packet.
 * @param mbuf Inbound ARP frame (consumed by this call).
 * @param out  Ring on which reply packets are enqueued.
 */
void arp_handle(struct rte_mempool *mp, struct rte_mbuf *mbuf,
                struct rte_ring *out);

#endif /* NETARCH_ARP_H */
