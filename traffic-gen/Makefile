# @file Makefile
# @brief Builds the DPDK traffic-generator executable and dependencies.
#
# The application links traffic-generator sources with the local protocol stack
# archive and llhttp sources.  `make` produces build/traffic-gen; `make clean`
# removes only artifacts owned by this directory.

# @brief Output binary name.
APP := traffic-gen
BUILD_DIR ?= build
STACK_BUILD_DIR ?= $(BUILD_DIR)
SANITIZERS ?=
STACK_LIB := ../pro-stack/$(STACK_BUILD_DIR)/libpro-stack.a
# @brief Default console level; override for protocol diagnostics.
LOG_LEVEL ?= LOG_LVL_WARN
# @brief Location of the vendored llhttp parser source tree.
LLHTTP_DIR := ../third_party/llhttp
# @brief Application and parser sources compiled into the executable.
SRCS := main_tg.c \
	core/reactor.c \
	core/conn_pool.c \
	core/flow.c \
	core/flow_pool.c \
	core/scenario.c \
	core/scenario_json.c \
	core/socket_capacity.c \
	core/scheduler.c \
	core/stats.c \
	core/stats_csv.c \
	core/txn.c \
	proto/registry.c \
	proto/http/http_client.c \
	proto/http/http_scenario.c \
	proto/dns/dns_client.c \
	proto/dns/dns_scenario.c \
	$(LLHTTP_DIR)/src/llhttp.c \
	$(LLHTTP_DIR)/src/api.c \
	$(LLHTTP_DIR)/src/http.c

# @brief pkg-config invocation used to discover the active DPDK installation.
PKGCONF := pkg-config --define-prefix
# @brief DPDK package configuration path retained for diagnostics and overrides.
DPDK_PC := $(shell $(PKGCONF) --path libdpdk)
# @brief Compiler flags for local headers, llhttp, and DPDK.
CFLAGS += -O3 -I. -Icore -Iproto -I../pro-stack -I$(LLHTTP_DIR)/include \
	$(shell $(PKGCONF) --cflags libdpdk)
CFLAGS += -DALLOW_EXPERIMENTAL_API
CFLAGS += -DLOG_LEVEL=$(LOG_LEVEL)
# @brief Static DPDK link flags resolved through pkg-config.
LDFLAGS := $(shell $(PKGCONF) --static --libs libdpdk)
ifneq ($(strip $(SANITIZERS)),)
CFLAGS += -O1 -g3 -fno-omit-frame-pointer -fno-optimize-sibling-calls
CFLAGS += -fsanitize=$(SANITIZERS)
LDFLAGS += -fsanitize=$(SANITIZERS)
endif

.PHONY: all clean
all: $(BUILD_DIR)/$(APP)

$(STACK_LIB):
	$(MAKE) -C ../pro-stack library LOG_LEVEL=$(LOG_LEVEL) \
		BUILD_DIR=$(STACK_BUILD_DIR) SANITIZERS=$(SANITIZERS)

$(BUILD_DIR)/$(APP): $(SRCS) Makefile $(STACK_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRCS) $(STACK_LIB) -o $@ $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -f $(BUILD_DIR)/$(APP)
	rmdir $(BUILD_DIR) 2>/dev/null || true
