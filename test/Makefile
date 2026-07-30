CC ?= cc

CFLAGS ?= -O2 -g
TEST_CFLAGS := $(CFLAGS) -std=c11 -Wall -Wextra -Werror -Wpedantic
# tcp.c is compiled as a test seam and contains existing GNU variadic logging
# macros, so build it with the project's GNU dialect without elevating its
# unrelated legacy warnings to test failures.
OFO_CFLAGS := $(CFLAGS) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE

DPDK_PKGCONF := pkg-config --define-prefix
DPDK_CFLAGS := $(shell $(DPDK_PKGCONF) --cflags libdpdk)
DPDK_LIBS := $(shell $(DPDK_PKGCONF) --libs libdpdk)

BUILD_DIR := build
RBTREE_TEST := $(BUILD_DIR)/test_rbtree
OFO_TEST := $(BUILD_DIR)/test_ofo

.PHONY: all test clean

all: test

# Compilation and execution both must succeed for the test target to pass.
test: $(RBTREE_TEST) $(OFO_TEST)
	$(RBTREE_TEST)
	$(OFO_TEST)

$(RBTREE_TEST): test_rbtree.c ../pro-stack/rbtree.c ../pro-stack/rbtree.h | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) test_rbtree.c ../pro-stack/rbtree.c -o $@

$(OFO_TEST): test_ofo.c ../pro-stack/tcp.c ../pro-stack/tcp.h \
		../pro-stack/rbtree.c ../pro-stack/rbtree.h | $(BUILD_DIR)
	$(CC) $(OFO_CFLAGS) $(DPDK_CFLAGS) -DTCP_TESTING -ffunction-sections \
		test_ofo.c ../pro-stack/tcp.c ../pro-stack/rbtree.c -o $@ \
		-Wl,--gc-sections $(DPDK_LIBS) -pthread

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
