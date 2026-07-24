/**
 * @file list.h
 * @brief Intrusive doubly linked-list helpers shared by protocol modules.
 */
#ifndef NETARCH_LIST_H
#define NETARCH_LIST_H

/** Insert an item containing prev/next fields at the list head. */
#define LL_ADD(item, list)                                                     \
        do {                                                                   \
                (item)->prev = NULL;                                           \
                (item)->next = (list);                                         \
                if ((list) != NULL)                                            \
                        (list)->prev = (item);                                 \
                (list) = (item);                                               \
        } while (0)

/** Remove an item containing prev/next fields from its list. */
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

#endif /* NETARCH_LIST_H */
