/**
 * @file rbtree.h
 * @brief Generic intrusive red-black tree primitives.
 *
 * The tree does not allocate objects or compare keys. Embed @ref rb_node in
 * the owning object, locate the insertion position using application-specific
 * ordering, then call rb_link_node() and rb_insert_color().
 */
#ifndef PRO_STACK_RBTREE_H
#define PRO_STACK_RBTREE_H

#include <stddef.h>

enum rb_color {
        RB_RED = 0,
        RB_BLACK = 1,
};

/** One intrusive red-black tree node. */
struct rb_node {
        struct rb_node *parent;
        struct rb_node *left;
        struct rb_node *right;
        enum rb_color color;
};

/** Root of an intrusive red-black tree. */
struct rb_root {
        struct rb_node *node;
};

#define RB_ROOT ((struct rb_root){.node = NULL})

/** Recover an owning object from one of its embedded tree nodes. */
#define rb_entry(ptr, type, member)                                            \
        ((type *)((char *)(ptr) - offsetof(type, member)))

/** Initialize an empty root. */
static inline void rb_root_init(struct rb_root *root) { root->node = NULL; }

/** Initialize a detached node before linking it into a tree. */
static inline void rb_node_init(struct rb_node *node) {
        node->parent = NULL;
        node->left = NULL;
        node->right = NULL;
        node->color = RB_RED;
}

/** Return non-zero when @p root has no nodes. */
static inline int rb_empty(const struct rb_root *root) {
        return root->node == NULL;
}

/**
 * Attach @p node at an application-selected vacant child link.
 *
 * Call rb_insert_color() immediately afterwards to restore the RB invariant.
 */
void rb_link_node(struct rb_node *node, struct rb_node *parent,
                  struct rb_node **link);

/** Restore RB invariants after rb_link_node(). */
void rb_insert_color(struct rb_node *node, struct rb_root *root);

/** Remove @p node from @p root and restore RB invariants. */
void rb_erase(struct rb_node *node, struct rb_root *root);

/** Return the lowest ordered node, or NULL for an empty tree. */
struct rb_node *rb_first(const struct rb_root *root);

/** Return the highest ordered node, or NULL for an empty tree. */
struct rb_node *rb_last(const struct rb_root *root);

/** Return the in-order successor of @p node, or NULL. */
struct rb_node *rb_next(const struct rb_node *node);

/** Return the in-order predecessor of @p node, or NULL. */
struct rb_node *rb_prev(const struct rb_node *node);

#endif /* NETARCH_RBTREE_H */
