CC ?= cc

CFLAGS ?= -O2 -g
TEST_CFLAGS := $(CFLAGS) -std=c11 -Wall -Wextra -Werror -Wpedantic
OWNER_IO_CFLAGS := $(TEST_CFLAGS) -D_GNU_SOURCE
# tcp.c is compiled as a test seam and contains existing GNU variadic logging
# macros, so build it with the project's GNU dialect without elevating its
# unrelated legacy warnings to test failures.
OFO_CFLAGS := $(CFLAGS) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -fdata-sections
PAWS_CFLAGS := $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE

DPDK_PKGCONF := pkg-config --define-prefix
DPDK_CFLAGS := $(shell $(DPDK_PKGCONF) --cflags libdpdk)
DPDK_LIBS := $(shell $(DPDK_PKGCONF) --libs libdpdk)

BUILD_DIR := build
RBTREE_TEST := $(BUILD_DIR)/test_rbtree
OFO_TEST := $(BUILD_DIR)/test_ofo
PAWS_TEST := $(BUILD_DIR)/test_tcp_paws
OWNER_IO_TEST := $(BUILD_DIR)/test_owner_io
HTTP_CLIENT_TEST := $(BUILD_DIR)/test_http_client
SCENARIO_SCHEDULER_TEST := $(BUILD_DIR)/test_scenario_scheduler
LLHTTP_DIR := ../third_party/llhttp
HTTP_CLIENT_CFLAGS := $(TEST_CFLAGS) -I../traffic-gen/core \
	-I../traffic-gen/proto -I../traffic-gen/proto/http \
	-I$(LLHTTP_DIR)/include
LLHTTP_CFLAGS := $(CFLAGS) -std=c11 -Wall -Wextra -Wno-unused-parameter \
	-I$(LLHTTP_DIR)/include
HTTP_CLIENT_OBJS := $(BUILD_DIR)/test_http_client.o \
	$(BUILD_DIR)/tg_txn.o \
	$(BUILD_DIR)/tg_http_client.o \
	$(BUILD_DIR)/llhttp.o \
	$(BUILD_DIR)/llhttp_api.o \
	$(BUILD_DIR)/llhttp_http.o
SCENARIO_SCHEDULER_OBJS := $(BUILD_DIR)/test_scenario_scheduler.o \
	$(BUILD_DIR)/tg_scenario.o \
	$(BUILD_DIR)/tg_scheduler.o \
	$(BUILD_DIR)/tg_stats.o \
	$(BUILD_DIR)/tg_http_client.o \
	$(BUILD_DIR)/llhttp.o \
	$(BUILD_DIR)/llhttp_api.o \
	$(BUILD_DIR)/llhttp_http.o

.PHONY: all test test-owner-io test-http-client test-scenario-scheduler clean

all: test

# Compilation and execution both must succeed for the test target to pass.
test: $(RBTREE_TEST) $(OFO_TEST) $(PAWS_TEST) $(OWNER_IO_TEST) \
	$(HTTP_CLIENT_TEST) $(SCENARIO_SCHEDULER_TEST)
	$(RBTREE_TEST)
	$(OFO_TEST)
	$(PAWS_TEST)
	$(OWNER_IO_TEST) --in-memory --no-huge
	$(HTTP_CLIENT_TEST)
	$(SCENARIO_SCHEDULER_TEST)

test-owner-io: $(OWNER_IO_TEST)

test-http-client: $(HTTP_CLIENT_TEST)

test-scenario-scheduler: $(SCENARIO_SCHEDULER_TEST)

$(RBTREE_TEST): test_rbtree.c ../pro-stack/rbtree.c ../pro-stack/rbtree.h | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) test_rbtree.c ../pro-stack/rbtree.c -o $@

$(OFO_TEST): test_ofo.c ../pro-stack/tcp.c ../pro-stack/tcp.h \
		../pro-stack/rbtree.c ../pro-stack/rbtree.h | $(BUILD_DIR)
	$(CC) $(OFO_CFLAGS) $(DPDK_CFLAGS) -DTCP_TESTING -ffunction-sections \
		test_ofo.c ../pro-stack/tcp.c ../pro-stack/rbtree.c -o $@ \
		-Wl,--gc-sections $(DPDK_LIBS) -pthread

$(PAWS_TEST): test_tcp_paws.c ../pro-stack/tcp_options.c \
		../pro-stack/tcp_options.h ../pro-stack/tcp.h | $(BUILD_DIR)
	$(CC) $(PAWS_CFLAGS) $(DPDK_CFLAGS) test_tcp_paws.c \
		../pro-stack/tcp_options.c -o $@ $(DPDK_LIBS)

$(OWNER_IO_TEST): test_owner_io.c ../pro-stack/build/libpro-stack.a | $(BUILD_DIR)
	$(CC) $(OWNER_IO_CFLAGS) $(DPDK_CFLAGS) -I../pro-stack test_owner_io.c \
		../pro-stack/build/libpro-stack.a -o $@ $(DPDK_LIBS)

$(HTTP_CLIENT_TEST): $(HTTP_CLIENT_OBJS)
	$(CC) $(HTTP_CLIENT_OBJS) -o $@

$(SCENARIO_SCHEDULER_TEST): $(SCENARIO_SCHEDULER_OBJS)
	$(CC) $(SCENARIO_SCHEDULER_OBJS) -o $@

$(BUILD_DIR)/test_http_client.o: test_http_client.c \
		../traffic-gen/core/txn.h \
		../traffic-gen/proto/http/http_client.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tg_txn.o: ../traffic-gen/core/txn.c \
		../traffic-gen/core/txn.h ../traffic-gen/proto/proto.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_scenario_scheduler.o: test_scenario_scheduler.c \
		../traffic-gen/core/scenario.h ../traffic-gen/core/scheduler.h \
		../traffic-gen/core/stats.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tg_scenario.o: ../traffic-gen/core/scenario.c \
		../traffic-gen/core/scenario.h ../third_party/jsmn/jsmn.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tg_scheduler.o: ../traffic-gen/core/scheduler.c \
		../traffic-gen/core/scheduler.h ../traffic-gen/core/scenario.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tg_stats.o: ../traffic-gen/core/stats.c \
		../traffic-gen/core/stats.h ../traffic-gen/core/flow.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tg_http_client.o: ../traffic-gen/proto/http/http_client.c \
		../traffic-gen/proto/http/http_client.h \
		../traffic-gen/core/txn.h $(LLHTTP_DIR)/include/llhttp.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/llhttp.o: $(LLHTTP_DIR)/src/llhttp.c \
		$(LLHTTP_DIR)/include/llhttp.h | $(BUILD_DIR)
	$(CC) $(LLHTTP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/llhttp_api.o: $(LLHTTP_DIR)/src/api.c \
		$(LLHTTP_DIR)/include/llhttp.h | $(BUILD_DIR)
	$(CC) $(LLHTTP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/llhttp_http.o: $(LLHTTP_DIR)/src/http.c \
		$(LLHTTP_DIR)/include/llhttp.h | $(BUILD_DIR)
	$(CC) $(LLHTTP_CFLAGS) -c $< -o $@

../pro-stack/build/libpro-stack.a:
	$(MAKE) -C ../pro-stack library

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
