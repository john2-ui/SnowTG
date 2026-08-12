# @file Makefile
# @brief Builds the DPDK traffic-generator executable and dependencies.
#
# The application links traffic-generator sources with the local protocol stack
# archive and llhttp sources.  `make` produces build/traffic-gen; `make clean`
# removes only artifacts owned by this directory.

# @brief Output binary name.
APP := traffic-gen
# @brief Location of the vendored llhttp parser source tree.
LLHTTP_DIR := ../third_party/llhttp
# @brief Application and parser sources compiled into the executable.
SRCS := main_tg.c \
	core/reactor.c \
	core/flow.c \
	core/flow_pool.c \
	core/scenario.c \
	core/scenario_json.c \
	core/socket_capacity.c \
	core/scheduler.c \
	core/stats.c \
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
# @brief Static DPDK link flags resolved through pkg-config.
LDFLAGS := $(shell $(PKGCONF) --static --libs libdpdk)

.PHONY: all clean
all: build/$(APP)

../pro-stack/build/libpro-stack.a:
	$(MAKE) -C ../pro-stack library

build/$(APP): $(SRCS) Makefile ../pro-stack/build/libpro-stack.a | build
	$(CC) $(CFLAGS) $(SRCS) ../pro-stack/build/libpro-stack.a -o $@ $(LDFLAGS)

build:
	mkdir -p $@

clean:
	rm -f build/$(APP)
	rmdir build 2>/dev/null || true
