CC ?= cc

CFLAGS ?= -O2 -g
BUILD_DIR ?= build
STACK_BUILD_DIR ?= $(BUILD_DIR)
SANITIZERS ?=
STACK_LIB := ../pro-stack/$(STACK_BUILD_DIR)/libpro-stack.a
ifneq ($(strip $(SANITIZERS)),)
CFLAGS += -O1 -g3 -fno-omit-frame-pointer -fno-optimize-sibling-calls
CFLAGS += -fsanitize=$(SANITIZERS)
SANITIZER_LDFLAGS := -fsanitize=$(SANITIZERS)
# These tests compile tcp.c as a seam.  At low optimization, production-only
# sections remain reachable, so use the instrumented stack archive to satisfy
# their dependencies instead of relying on --gc-sections to discard them.
TCP_SEAM_SUPPORT := $(STACK_LIB)
TCP_SEAM_LDFLAGS := -Wl,--allow-multiple-definition
endif
TEST_CFLAGS := $(CFLAGS) -std=c11 -Wall -Wextra -Werror -Wpedantic
OWNER_IO_CFLAGS := $(TEST_CFLAGS) -D_GNU_SOURCE
RX_DISPATCH_CFLAGS := $(TEST_CFLAGS) -D_GNU_SOURCE
# tcp.c is compiled as a test seam and contains existing GNU variadic logging
# macros, so build it with the project's GNU dialect without elevating its
# unrelated legacy warnings to test failures.
OFO_CFLAGS := $(CFLAGS) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -fdata-sections
PAWS_CFLAGS := $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE
ARP_CFLAGS := $(PAWS_CFLAGS) -DLOG_LEVEL=LOG_LVL_ERROR -DARP_TESTING

DPDK_PKGCONF := pkg-config --define-prefix
DPDK_CFLAGS := $(shell $(DPDK_PKGCONF) --cflags libdpdk)
DPDK_LIBS := $(shell $(DPDK_PKGCONF) --libs libdpdk)

RBTREE_TEST := $(BUILD_DIR)/test_rbtree
OFO_TEST := $(BUILD_DIR)/test_ofo
PAWS_TEST := $(BUILD_DIR)/test_tcp_paws
SACK_TEST := $(BUILD_DIR)/test_tcp_sack
TCP_CC_TEST := $(BUILD_DIR)/test_tcp_cc
EPHEMERAL_TEST := $(BUILD_DIR)/test_tcp_ephemeral
OWNER_IO_TEST := $(BUILD_DIR)/test_owner_io
HTTP_CLIENT_TEST := $(BUILD_DIR)/test_http_client
DNS_CLIENT_TEST := $(BUILD_DIR)/test_dns_client
SCENARIO_SCHEDULER_TEST := $(BUILD_DIR)/test_scenario_scheduler
ARP_TEST := $(BUILD_DIR)/test_arp
TCP_CC_SRCS := ../pro-stack/tcp_cc.c ../pro-stack/tcp_cc/newreno.c \
	../pro-stack/tcp_cc/cubic.c
RX_DISPATCH_TEST := $(BUILD_DIR)/test_rx_dispatch
PORT_TOPOLOGY_TEST := $(BUILD_DIR)/test_port_topology
FLOW_UDP_TEST := $(BUILD_DIR)/test_flow_udp
IPV4_REASSEMBLY_TEST := $(BUILD_DIR)/test_ipv4_reassembly
STATS_CSV_TEST := $(BUILD_DIR)/test_stats_csv
CONN_POOL_TEST := $(BUILD_DIR)/test_conn_pool
LLHTTP_DIR := ../third_party/llhttp
HTTP_CLIENT_CFLAGS := $(TEST_CFLAGS) -I../traffic-gen/core \
	-I../traffic-gen/proto -I../traffic-gen/proto/http \
	-I$(LLHTTP_DIR)/include
DNS_CLIENT_CFLAGS := $(TEST_CFLAGS) -I../traffic-gen/core \
	-I../traffic-gen/proto -I../traffic-gen/proto/dns
LLHTTP_CFLAGS := $(CFLAGS) -std=c11 -Wall -Wextra -Wno-unused-parameter \
	-I$(LLHTTP_DIR)/include
HTTP_CLIENT_OBJS := $(BUILD_DIR)/test_http_client.o \
	$(BUILD_DIR)/tg_txn.o \
	$(BUILD_DIR)/tg_http_client.o \
	$(BUILD_DIR)/llhttp.o \
	$(BUILD_DIR)/llhttp_api.o \
	$(BUILD_DIR)/llhttp_http.o
DNS_CLIENT_OBJS := $(BUILD_DIR)/test_dns_client.o \
	$(BUILD_DIR)/tg_txn.o \
	$(BUILD_DIR)/tg_dns_client.o
SCENARIO_SCHEDULER_OBJS := $(BUILD_DIR)/test_scenario_scheduler.o \
	$(BUILD_DIR)/tg_scenario.o \
	$(BUILD_DIR)/tg_scenario_json.o \
	$(BUILD_DIR)/tg_socket_capacity.o \
	$(BUILD_DIR)/tg_registry.o \
	$(BUILD_DIR)/tg_scheduler.o \
	$(BUILD_DIR)/tg_stats.o \
	$(BUILD_DIR)/tg_http_client.o \
	$(BUILD_DIR)/tg_http_scenario.o \
	$(BUILD_DIR)/tg_dns_client.o \
	$(BUILD_DIR)/tg_dns_scenario.o \
	$(BUILD_DIR)/llhttp.o \
	$(BUILD_DIR)/llhttp_api.o \
	$(BUILD_DIR)/llhttp_http.o

.PHONY: all test test-arp test-owner-io test-http-client test-dns-client \
	test-scenario-scheduler \
	test-rx-dispatch test-port-topology test-flow-udp test-ipv4-reassembly \
	test-stats-csv test-conn-pool \
	test-tcp-ephemeral test-tcp-sack test-tcp-cc clean

all: test

# Compilation and execution both must succeed for the test target to pass.
test: $(RBTREE_TEST) $(OFO_TEST) $(PAWS_TEST) $(SACK_TEST) $(TCP_CC_TEST) $(EPHEMERAL_TEST) $(OWNER_IO_TEST) \
	$(HTTP_CLIENT_TEST) $(DNS_CLIENT_TEST) $(SCENARIO_SCHEDULER_TEST) $(ARP_TEST) \
	$(RX_DISPATCH_TEST) $(PORT_TOPOLOGY_TEST) $(FLOW_UDP_TEST) \
	$(IPV4_REASSEMBLY_TEST) $(STATS_CSV_TEST) $(CONN_POOL_TEST)
	$(RBTREE_TEST)
	$(OFO_TEST)
	$(PAWS_TEST)
	$(SACK_TEST)
	$(TCP_CC_TEST)
	$(EPHEMERAL_TEST)
	$(OWNER_IO_TEST) --no-huge --no-pci --file-prefix ownerio-test
	$(HTTP_CLIENT_TEST)
	$(DNS_CLIENT_TEST)
	$(SCENARIO_SCHEDULER_TEST)
	$(ARP_TEST) --no-huge --no-pci --file-prefix arp-test
	$(RX_DISPATCH_TEST)
	$(PORT_TOPOLOGY_TEST)
	$(FLOW_UDP_TEST) --no-huge --no-pci --file-prefix flow-udp-test
	$(IPV4_REASSEMBLY_TEST) --no-huge --no-pci --file-prefix ipv4-reassembly-test
	$(STATS_CSV_TEST)
	$(CONN_POOL_TEST)

test-arp: $(ARP_TEST)

test-owner-io: $(OWNER_IO_TEST)

test-http-client: $(HTTP_CLIENT_TEST)

test-dns-client: $(DNS_CLIENT_TEST)

test-scenario-scheduler: $(SCENARIO_SCHEDULER_TEST)

test-tcp-ephemeral: $(EPHEMERAL_TEST)

test-tcp-sack: $(SACK_TEST)

test-tcp-cc: $(TCP_CC_TEST)

test-rx-dispatch: $(RX_DISPATCH_TEST)

test-port-topology: $(PORT_TOPOLOGY_TEST)

test-flow-udp: $(FLOW_UDP_TEST)

test-ipv4-reassembly: $(IPV4_REASSEMBLY_TEST)

test-stats-csv: $(STATS_CSV_TEST)

test-conn-pool: $(CONN_POOL_TEST)

$(RBTREE_TEST): test_rbtree.c ../pro-stack/rbtree.c ../pro-stack/rbtree.h | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) test_rbtree.c ../pro-stack/rbtree.c -o $@

$(OFO_TEST): test_ofo.c ../pro-stack/tcp.c ../pro-stack/tcp.h \
		../pro-stack/tcp_sack.c $(TCP_CC_SRCS) \
		../pro-stack/rbtree.c ../pro-stack/rbtree.h $(TCP_SEAM_SUPPORT) | $(BUILD_DIR)
	$(CC) $(OFO_CFLAGS) $(DPDK_CFLAGS) -DTCP_TESTING -ffunction-sections \
		test_ofo.c ../pro-stack/tcp.c ../pro-stack/tcp_sack.c \
		$(TCP_CC_SRCS) ../pro-stack/rbtree.c -o $@ \
		-Wl,--gc-sections $(TCP_SEAM_LDFLAGS) $(TCP_SEAM_SUPPORT) $(DPDK_LIBS) -pthread

$(PAWS_TEST): test_tcp_paws.c ../pro-stack/tcp_options.c \
		../pro-stack/tcp_sack.c ../pro-stack/tcp_options.h \
		../pro-stack/tcp.h | $(BUILD_DIR)
	$(CC) $(PAWS_CFLAGS) $(DPDK_CFLAGS) -DTCP_TESTING test_tcp_paws.c \
		../pro-stack/tcp_options.c ../pro-stack/tcp_sack.c -o $@ $(DPDK_LIBS)

$(SACK_TEST): test_tcp_sack.c ../pro-stack/tcp.c ../pro-stack/tcp_options.c \
		../pro-stack/tcp_sack.c $(TCP_CC_SRCS) ../pro-stack/tcp_rtt.c ../pro-stack/tcp.h \
		../pro-stack/tcp_options.h ../pro-stack/rbtree.c | \
		$(TCP_SEAM_SUPPORT) $(BUILD_DIR)
	$(CC) $(OFO_CFLAGS) $(DPDK_CFLAGS) -DTCP_TESTING -ffunction-sections \
		test_tcp_sack.c ../pro-stack/tcp.c ../pro-stack/tcp_options.c \
		../pro-stack/tcp_sack.c $(TCP_CC_SRCS) ../pro-stack/tcp_rtt.c ../pro-stack/rbtree.c \
		-o $@ -Wl,--gc-sections $(TCP_SEAM_LDFLAGS) $(TCP_SEAM_SUPPORT) $(DPDK_LIBS) -pthread

$(TCP_CC_TEST): test_tcp_cc.c $(TCP_CC_SRCS) ../pro-stack/tcp_cc.h \
		../pro-stack/tcp_cc/newreno.h ../pro-stack/tcp_cc/cubic.h | $(BUILD_DIR)
	$(CC) $(OFO_CFLAGS) $(DPDK_CFLAGS) -DTCP_TESTING -I../pro-stack test_tcp_cc.c \
		$(TCP_CC_SRCS) -o $@ $(DPDK_LIBS)

$(EPHEMERAL_TEST): test_tcp_ephemeral.c ../pro-stack/tcp.c \
		../pro-stack/tcp_sack.c $(TCP_CC_SRCS) ../pro-stack/tcp.h \
		../pro-stack/rbtree.c ../pro-stack/rbtree.h | \
		$(TCP_SEAM_SUPPORT) $(BUILD_DIR)
	$(CC) $(OFO_CFLAGS) $(DPDK_CFLAGS) -DTCP_TESTING -ffunction-sections \
		test_tcp_ephemeral.c ../pro-stack/tcp.c ../pro-stack/tcp_sack.c \
		$(TCP_CC_SRCS) ../pro-stack/rbtree.c -o $@ \
		-Wl,--gc-sections $(TCP_SEAM_LDFLAGS) $(TCP_SEAM_SUPPORT) $(DPDK_LIBS) -pthread

$(OWNER_IO_TEST): test_owner_io.c $(STACK_LIB) | $(BUILD_DIR)
	$(CC) $(OWNER_IO_CFLAGS) $(DPDK_CFLAGS) -I../pro-stack test_owner_io.c \
		$(STACK_LIB) -o $@ $(DPDK_LIBS) $(SANITIZER_LDFLAGS)

$(ARP_TEST): test_arp.c ../pro-stack/arp.c ../pro-stack/arp.h \
		../pro-stack/net_context.c ../pro-stack/net_context.h | $(BUILD_DIR)
	$(CC) $(ARP_CFLAGS) $(DPDK_CFLAGS) -I../pro-stack test_arp.c \
		../pro-stack/arp.c ../pro-stack/net_context.c -o $@ $(DPDK_LIBS)

$(RX_DISPATCH_TEST): test_rx_dispatch.c ../pro-stack/rx_dispatch.c \
		../pro-stack/rx_dispatch.h ../pro-stack/port.c ../pro-stack/port.h | $(BUILD_DIR)
	$(CC) $(RX_DISPATCH_CFLAGS) $(DPDK_CFLAGS) -DPORT_TESTING -I../pro-stack \
		test_rx_dispatch.c ../pro-stack/rx_dispatch.c ../pro-stack/port.c \
		-o $@ $(DPDK_LIBS) -pthread

$(PORT_TOPOLOGY_TEST): test_port_topology.c ../pro-stack/port.c \
		../pro-stack/port.h ../pro-stack/config.h | $(BUILD_DIR)
	$(CC) $(RX_DISPATCH_CFLAGS) $(DPDK_CFLAGS) -DPORT_TESTING -I../pro-stack \
		test_port_topology.c ../pro-stack/port.c -o $@ $(DPDK_LIBS)

$(FLOW_UDP_TEST): test_flow_udp.c ../traffic-gen/core/conn_pool.c \
		../traffic-gen/core/flow.c \
		../traffic-gen/core/flow_pool.c ../traffic-gen/core/txn.c \
		../pro-stack/arp.h ../pro-stack/config.h ../pro-stack/ring.h \
		../pro-stack/udp.h $(STACK_LIB) | $(BUILD_DIR)
	$(CC) $(OWNER_IO_CFLAGS) $(DPDK_CFLAGS) \
		-I../pro-stack -I../traffic-gen/core -I../traffic-gen/proto \
		test_flow_udp.c ../traffic-gen/core/conn_pool.c \
		../traffic-gen/core/flow.c \
		../traffic-gen/core/flow_pool.c ../traffic-gen/core/txn.c \
		$(STACK_LIB) -o $@ $(DPDK_LIBS) $(SANITIZER_LDFLAGS)

$(IPV4_REASSEMBLY_TEST): test_ipv4_reassembly.c \
		../pro-stack/ipv4_reassembly.h ../pro-stack/udp.h \
		$(STACK_LIB) | $(BUILD_DIR)
	$(CC) $(OWNER_IO_CFLAGS) $(DPDK_CFLAGS) -I../pro-stack \
		test_ipv4_reassembly.c $(STACK_LIB) \
		-o $@ $(DPDK_LIBS) $(SANITIZER_LDFLAGS)

$(STATS_CSV_TEST): test_stats_csv.c ../traffic-gen/core/stats_csv.c \
		../traffic-gen/core/stats_csv.h ../traffic-gen/core/stats.h | $(BUILD_DIR)
	$(CC) $(OWNER_IO_CFLAGS) $(DPDK_CFLAGS) -I../traffic-gen/core \
		test_stats_csv.c ../traffic-gen/core/stats_csv.c -o $@ $(DPDK_LIBS)

$(CONN_POOL_TEST): test_conn_pool.c ../traffic-gen/core/conn_pool.c \
		../traffic-gen/core/conn_pool.h ../traffic-gen/core/flow.h | \
		$(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -I../pro-stack -I../traffic-gen/core \
		-I../traffic-gen/proto test_conn_pool.c \
		../traffic-gen/core/conn_pool.c -o $@

$(HTTP_CLIENT_TEST): $(HTTP_CLIENT_OBJS)
	$(CC) $(HTTP_CLIENT_OBJS) -o $@ $(SANITIZER_LDFLAGS)

$(DNS_CLIENT_TEST): $(DNS_CLIENT_OBJS)
	$(CC) $(DNS_CLIENT_OBJS) -o $@ $(SANITIZER_LDFLAGS)

$(SCENARIO_SCHEDULER_TEST): $(SCENARIO_SCHEDULER_OBJS)
	$(CC) $(SCENARIO_SCHEDULER_OBJS) -o $@ $(SANITIZER_LDFLAGS)

$(BUILD_DIR)/test_http_client.o: test_http_client.c \
		../traffic-gen/core/txn.h \
		../traffic-gen/proto/http/http_client.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_dns_client.o: test_dns_client.c \
		../traffic-gen/core/txn.h \
		../traffic-gen/proto/dns/dns_client.h | $(BUILD_DIR)
	$(CC) $(DNS_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tg_txn.o: ../traffic-gen/core/txn.c \
		../traffic-gen/core/txn.h ../traffic-gen/proto/proto.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_scenario_scheduler.o: test_scenario_scheduler.c \
		../traffic-gen/core/scenario.h ../traffic-gen/core/scheduler.h \
		../traffic-gen/core/socket_capacity.h \
		../traffic-gen/core/stats.h \
		../traffic-gen/proto/http/http_client.h \
		../traffic-gen/proto/dns/dns_client.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tg_scenario.o: ../traffic-gen/core/scenario.c \
		../traffic-gen/core/scenario.h ../traffic-gen/core/scenario_json.h \
		../traffic-gen/proto/registry.h \
		../traffic-gen/proto/http/http_client.h \
		../traffic-gen/proto/dns/dns_client.h \
		../third_party/jsmn/jsmn.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tg_scenario_json.o: ../traffic-gen/core/scenario_json.c \
		../traffic-gen/core/scenario_json.h ../third_party/jsmn/jsmn.h | \
		$(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tg_socket_capacity.o: ../traffic-gen/core/socket_capacity.c \
		../traffic-gen/core/socket_capacity.h \
		../traffic-gen/core/scenario.h \
		../pro-stack/socket_owner.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tg_registry.o: ../traffic-gen/proto/registry.c \
		../traffic-gen/proto/registry.h \
		../traffic-gen/proto/http/http_scenario.h \
		../traffic-gen/proto/dns/dns_scenario.h | $(BUILD_DIR)
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

$(BUILD_DIR)/tg_http_scenario.o: \
		../traffic-gen/proto/http/http_scenario.c \
		../traffic-gen/proto/http/http_scenario.h \
		../traffic-gen/proto/http/http_client.h \
		../traffic-gen/core/scenario.h | $(BUILD_DIR)
	$(CC) $(HTTP_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tg_dns_client.o: ../traffic-gen/proto/dns/dns_client.c \
		../traffic-gen/proto/dns/dns_client.h \
		../traffic-gen/core/txn.h | $(BUILD_DIR)
	$(CC) $(DNS_CLIENT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tg_dns_scenario.o: \
		../traffic-gen/proto/dns/dns_scenario.c \
		../traffic-gen/proto/dns/dns_scenario.h \
		../traffic-gen/proto/dns/dns_client.h \
		../traffic-gen/core/scenario.h | $(BUILD_DIR)
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

$(STACK_LIB):
	$(MAKE) -C ../pro-stack library BUILD_DIR=$(STACK_BUILD_DIR) \
		SANITIZERS=$(SANITIZERS)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
