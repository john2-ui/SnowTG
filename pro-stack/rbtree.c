/**
 * @file rbtree.c
 * @brief Generic intrusive red-black tree balancing and traversal.
 */
#include "rbtree.h"

/** @brief Return a node color, treating NULL leaves as black. */
static enum rb_color rb_color_of(const struct rb_node *node) {
        return node == NULL ? RB_BLACK : node->color;
}

/** @brief Set a node color when the node is non-NULL. */
static void rb_set_color(struct rb_node *node, enum rb_color color) {
        if (node != NULL)
                node->color = color;
}

/** @brief Return the leftmost node in a non-empty subtree. */
static struct rb_node *rb_minimum(struct rb_node *node) {
        while (node != NULL && node->left != NULL)
                node = node->left;
        return node;
}

/** @brief Return the rightmost node in a non-empty subtree. */
static struct rb_node *rb_maximum(struct rb_node *node) {
        while (node != NULL && node->right != NULL)
                node = node->right;
        return node;
}

/** @brief Rotate @p node left, promoting its right child. */
static void rb_rotate_left(struct rb_node *node, struct rb_root *root) {
        struct rb_node *right = node->right;

        node->right = right->left;
        if (right->left != NULL)
                right->left->parent = node;

        right->parent = node->parent;
        if (node->parent == NULL)
                root->node = right;
        else if (node == node->parent->left)
                node->parent->left = right;
        else
                node->parent->right = right;

        right->left = node;
        node->parent = right;
}

/** @brief Rotate @p node right, promoting its left child. */
static void rb_rotate_right(struct rb_node *node, struct rb_root *root) {
        struct rb_node *left = node->left;

        node->left = left->right;
        if (left->right != NULL)
                left->right->parent = node;

        left->parent = node->parent;
        if (node->parent == NULL)
                root->node = left;
        else if (node == node->parent->right)
                node->parent->right = left;
        else
                node->parent->left = left;

        left->right = node;
        node->parent = left;
}

void rb_link_node(struct rb_node *node, struct rb_node *parent,
                  struct rb_node **link) {
        node->parent = parent;
        node->left = NULL;
        node->right = NULL;
        node->color = RB_RED;
        *link = node;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root) {
        while (node != root->node && node->parent->color == RB_RED) {
                struct rb_node *parent = node->parent;
                struct rb_node *grandparent = parent->parent;

                if (parent == grandparent->left) {
                        struct rb_node *uncle = grandparent->right;

                        if (rb_color_of(uncle) == RB_RED) {
                                parent->color = RB_BLACK;
                                uncle->color = RB_BLACK;
                                grandparent->color = RB_RED;
                                node = grandparent;
                                continue;
                        }

                        if (node == parent->right) {
                                node = parent;
                                rb_rotate_left(node, root);
                                parent = node->parent;
                                grandparent = parent->parent;
                        }

                        parent->color = RB_BLACK;
                        grandparent->color = RB_RED;
                        rb_rotate_right(grandparent, root);
                } else {
                        struct rb_node *uncle = grandparent->left;

                        if (rb_color_of(uncle) == RB_RED) {
                                parent->color = RB_BLACK;
                                uncle->color = RB_BLACK;
                                grandparent->color = RB_RED;
                                node = grandparent;
                                continue;
                        }

                        if (node == parent->left) {
                                node = parent;
                                rb_rotate_right(node, root);
                                parent = node->parent;
                                grandparent = parent->parent;
                        }

                        parent->color = RB_BLACK;
                        grandparent->color = RB_RED;
                        rb_rotate_left(grandparent, root);
                }
        }

        root->node->color = RB_BLACK;
}

/**
 * @brief Replace one subtree root with another without changing colors.
 * @param root Tree being modified.
 * @param old Existing subtree root.
 * @param new Replacement subtree root, which may be NULL.
 */
static void rb_transplant(struct rb_root *root, struct rb_node *old,
                          struct rb_node *new) {
        if (old->parent == NULL)
                root->node = new;
        else if (old == old->parent->left)
                old->parent->left = new;
        else
                old->parent->right = new;

        if (new != NULL)
                new->parent = old->parent;
}

/**
 * @brief Restore RB invariants after removal of a black node.
 * @param node Replacement node, possibly NULL.
 * @param parent Parent of @p node when it is NULL.
 * @param root Tree being rebalanced.
 */
static void rb_erase_fixup(struct rb_node *node, struct rb_node *parent,
                           struct rb_root *root) {
        while (node != root->node && rb_color_of(node) == RB_BLACK) {
                if (node == parent->left) {
                        struct rb_node *sibling = parent->right;

                        if (rb_color_of(sibling) == RB_RED) {
                                sibling->color = RB_BLACK;
                                parent->color = RB_RED;
                                rb_rotate_left(parent, root);
                                sibling = parent->right;
                        }

                        if (sibling == NULL) {
                                node = parent;
                                parent = node->parent;
                                continue;
                        }

                        if (rb_color_of(sibling->left) == RB_BLACK &&
                            rb_color_of(sibling->right) == RB_BLACK) {
                                sibling->color = RB_RED;
                                node = parent;
                                parent = node->parent;
                        } else {
                                if (rb_color_of(sibling->right) == RB_BLACK) {
                                        rb_set_color(sibling->left, RB_BLACK);
                                        sibling->color = RB_RED;
                                        rb_rotate_right(sibling, root);
                                        sibling = parent->right;
                                }

                                sibling->color = parent->color;
                                parent->color = RB_BLACK;
                                rb_set_color(sibling->right, RB_BLACK);
                                rb_rotate_left(parent, root);
                                node = root->node;
                                parent = NULL;
                        }
                } else {
                        struct rb_node *sibling = parent->left;

                        if (rb_color_of(sibling) == RB_RED) {
                                sibling->color = RB_BLACK;
                                parent->color = RB_RED;
                                rb_rotate_right(parent, root);
                                sibling = parent->left;
                        }

                        if (sibling == NULL) {
                                node = parent;
                                parent = node->parent;
                                continue;
                        }

                        if (rb_color_of(sibling->right) == RB_BLACK &&
                            rb_color_of(sibling->left) == RB_BLACK) {
                                sibling->color = RB_RED;
                                node = parent;
                                parent = node->parent;
                        } else {
                                if (rb_color_of(sibling->left) == RB_BLACK) {
                                        rb_set_color(sibling->right, RB_BLACK);
                                        sibling->color = RB_RED;
                                        rb_rotate_left(sibling, root);
                                        sibling = parent->left;
                                }

                                sibling->color = parent->color;
                                parent->color = RB_BLACK;
                                rb_set_color(sibling->left, RB_BLACK);
                                rb_rotate_right(parent, root);
                                node = root->node;
                                parent = NULL;
                        }
                }
        }

        rb_set_color(node, RB_BLACK);
}

void rb_erase(struct rb_node *node, struct rb_root *root) {
        struct rb_node *replacement;
        struct rb_node *replacement_parent;
        struct rb_node *removed = node;
        enum rb_color removed_color = removed->color;

        if (node->left == NULL) {
                replacement = node->right;
                replacement_parent = node->parent;
                rb_transplant(root, node, node->right);
        } else if (node->right == NULL) {
                replacement = node->left;
                replacement_parent = node->parent;
                rb_transplant(root, node, node->left);
        } else {
                removed = rb_minimum(node->right);
                removed_color = removed->color;
                replacement = removed->right;

                if (removed->parent == node) {
                        replacement_parent = removed;
                        if (replacement != NULL)
                                replacement->parent = removed;
                } else {
                        replacement_parent = removed->parent;
                        rb_transplant(root, removed, removed->right);
                        removed->right = node->right;
                        removed->right->parent = removed;
                }

                rb_transplant(root, node, removed);
                removed->left = node->left;
                removed->left->parent = removed;
                removed->color = node->color;
        }

        if (removed_color == RB_BLACK)
                rb_erase_fixup(replacement, replacement_parent, root);

        rb_node_init(node);
}

struct rb_node *rb_first(const struct rb_root *root) {
        return rb_minimum(root->node);
}

struct rb_node *rb_last(const struct rb_root *root) {
        return rb_maximum(root->node);
}

struct rb_node *rb_next(const struct rb_node *node) {
        const struct rb_node *parent;

        if (node == NULL)
                return NULL;
        if (node->right != NULL)
                return rb_minimum(node->right);

        parent = node->parent;
        while (parent != NULL && node == parent->right) {
                node = parent;
                parent = parent->parent;
        }
        return (struct rb_node *)parent;
}

struct rb_node *rb_prev(const struct rb_node *node) {
        const struct rb_node *parent;

        if (node == NULL)
                return NULL;
        if (node->left != NULL)
                return rb_maximum(node->left);

        parent = node->parent;
        while (parent != NULL && node == parent->left) {
                node = parent;
                parent = parent->parent;
        }
        return (struct rb_node *)parent;
}
