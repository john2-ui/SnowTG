#include "../pro-stack/rbtree.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,         \
              #condition);                                                     \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

struct test_node {
  int key;
  struct rb_node rb;
};

static void tree_insert(struct rb_root *root, struct test_node *entry) {
  struct rb_node **link = &root->node;
  struct rb_node *parent = NULL;

  while (*link != NULL) {
    struct test_node *current = rb_entry(*link, struct test_node, rb);

    parent = *link;
    if (entry->key < current->key)
      link = &(*link)->left;
    else
      link = &(*link)->right;
  }

  rb_link_node(&entry->rb, parent, link);
  rb_insert_color(&entry->rb, root);
}

static int validate_subtree(const struct rb_node *node,
                            const struct rb_node *parent, int min_key,
                            int max_key) {
  const struct test_node *entry;
  int left_height;
  int right_height;

  if (node == NULL)
    return 1; /* NULL leaves are black. */

  entry = rb_entry(node, struct test_node, rb);
  CHECK(node->parent == parent);
  CHECK(entry->key > min_key && entry->key < max_key);
  CHECK(node->color == RB_RED || node->color == RB_BLACK);
  if (node->color == RB_RED) {
    CHECK(node->left == NULL || node->left->color == RB_BLACK);
    CHECK(node->right == NULL || node->right->color == RB_BLACK);
  }

  left_height = validate_subtree(node->left, node, min_key, entry->key);
  right_height = validate_subtree(node->right, node, entry->key, max_key);
  CHECK(left_height == right_height);
  return left_height + (node->color == RB_BLACK ? 1 : 0);
}

static size_t count_present(const bool *present, size_t count) {
  size_t i;
  size_t total = 0;

  for (i = 0; i < count; i++)
    total += present[i] ? 1U : 0U;
  return total;
}

static void validate_tree(const struct rb_root *root, const bool *present,
                          size_t count) {
  struct rb_node *node;
  size_t index = 0;

  if (root->node != NULL)
    CHECK(root->node->color == RB_BLACK);
  (void)validate_subtree(root->node, NULL, -1, (int)count);

  for (node = rb_first(root); node != NULL; node = rb_next(node)) {
    const struct test_node *entry = rb_entry(node, struct test_node, rb);
    CHECK(index < count);
    CHECK(present[entry->key]);
    if (index != 0) {
      const struct test_node *previous =
          rb_entry(rb_prev(node), struct test_node, rb);
      CHECK(previous->key < entry->key);
    } else {
      CHECK(rb_prev(node) == NULL);
    }
    index++;
  }
  CHECK(index > 0 || rb_empty(root));

  index = 0;
  for (node = rb_last(root); node != NULL; node = rb_prev(node)) {
    const struct test_node *entry = rb_entry(node, struct test_node, rb);
    CHECK(index < count);
    CHECK(present[entry->key]);
    if (index == 0)
      CHECK(rb_next(node) == NULL);
    index++;
  }
  CHECK(index == count_present(present, count));
}

static void test_empty_tree(void) {
  struct rb_root root = RB_ROOT;

  CHECK(rb_empty(&root));
  CHECK(rb_first(&root) == NULL);
  CHECK(rb_last(&root) == NULL);
  CHECK(rb_next(NULL) == NULL);
  CHECK(rb_prev(NULL) == NULL);
}

static void test_insert_and_erase_all_orders(void) {
  enum { NODE_COUNT = 128 };
  struct test_node nodes[NODE_COUNT];
  bool present[NODE_COUNT] = {false};
  struct rb_root root = RB_ROOT;
  size_t i;

  for (i = 0; i < NODE_COUNT; i++) {
    nodes[i].key = (int)i;
    rb_node_init(&nodes[i].rb);
    tree_insert(&root, &nodes[i]);
    present[i] = true;
    validate_tree(&root, present, NODE_COUNT);
  }

  for (i = NODE_COUNT; i-- > 0;) {
    rb_erase(&nodes[i].rb, &root);
    present[i] = false;
    CHECK(nodes[i].rb.parent == NULL && nodes[i].rb.left == NULL &&
          nodes[i].rb.right == NULL);
    validate_tree(&root, present, NODE_COUNT);
  }
  CHECK(rb_empty(&root));
}

static void test_mixed_insert_and_erase(void) {
  static const unsigned int insertion_order[] = {
      19, 3,  25, 7,  0, 31, 11, 4,  28, 15, 1,  30, 9,  23, 5,  17,
      2,  29, 8,  22, 6, 16, 10, 24, 12, 18, 13, 20, 14, 21, 26, 27,
  };
  static const unsigned int erase_order[] = {
      19, 0,  31, 7,  25, 3,  15, 28, 4,  11, 30, 1,  23, 9,  5,  17,
      2,  29, 8,  22, 6,  16, 10, 24, 12, 18, 13, 20, 14, 21, 26, 27,
  };
  struct test_node nodes[ARRAY_SIZE(insertion_order)];
  bool present[ARRAY_SIZE(insertion_order)] = {false};
  struct rb_root root = RB_ROOT;
  size_t i;

  for (i = 0; i < ARRAY_SIZE(insertion_order); i++) {
    unsigned int key = insertion_order[i];

    nodes[key].key = (int)key;
    rb_node_init(&nodes[key].rb);
    tree_insert(&root, &nodes[key]);
    present[key] = true;
    validate_tree(&root, present, ARRAY_SIZE(nodes));
  }
  for (i = 0; i < ARRAY_SIZE(erase_order); i++) {
    unsigned int key = erase_order[i];

    rb_erase(&nodes[key].rb, &root);
    present[key] = false;
    validate_tree(&root, present, ARRAY_SIZE(nodes));
  }
}

int main(void) {
  test_empty_tree();
  test_insert_and_erase_all_orders();
  test_mixed_insert_and_erase();
  puts("test_rbtree: PASS");
  return EXIT_SUCCESS;
}
