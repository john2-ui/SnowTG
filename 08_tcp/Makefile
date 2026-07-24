# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2010-2014 Intel Corporation

# binary name
APP = dpdk_tcp

# all source are stored in SRCS-y
SRCS-y := main.c net_context.c ring.c arp.c udp.c icmp.c port.c \
	socket.c pkt_frame.c udp_app.c tcp.c

# Build using pkg-config variables if possible
ifeq ($(shell pkg-config --exists libdpdk && echo 0),0)

# dpdk-pdump installed by this DPDK build is statically linked. Use the same
# DPDK linkage mode and PMD registration order in the primary process.
all: static
.PHONY: shared static
shared: build/$(APP)-shared
	ln -sf $(APP)-shared build/$(APP)
static: build/$(APP)-static
	ln -sf $(APP)-static build/$(APP)

PKGCONF=pkg-config --define-prefix

PC_FILE := $(shell $(PKGCONF) --path libdpdk)
CFLAGS += -O3 $(shell $(PKGCONF) --cflags libdpdk)
CFLAGS += -DALLOW_EXPERIMENTAL_API
# Force --no-as-needed so ALL DPDK libraries are linked, not just the ones the
# app references directly. This makes every library's constructor register its
# tailq, which a full secondary process (e.g. dpdk-pdump) requires; otherwise
# the secondary panics with "Cannot init tail queues for objects".
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk | sed 's/-Wl,--as-needed/-Wl,--no-as-needed/')
# pkg-config already names DPDK archives explicitly (librte_*.a). Do not add
# -Bstatic globally: system dependencies such as libc and libgcc stay dynamic.
LDFLAGS_STATIC = $(shell $(PKGCONF) --static --libs libdpdk)

build/$(APP)-shared: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

build/$(APP)-static: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_STATIC)

build:
	@mkdir -p $@

.PHONY: clean
clean:
	rm -f build/$(APP) build/$(APP)-static build/$(APP)-shared
	test -d build && rmdir -p build || true

else # Build using legacy build system

ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

# Default target, detect a build directory, by looking for a path with a .config
RTE_TARGET ?= $(notdir $(abspath $(dir $(firstword $(wildcard $(RTE_SDK)/*/.config)))))

include $(RTE_SDK)/mk/rte.vars.mk

ifneq ($(CONFIG_RTE_EXEC_ENV_LINUX),y)
$(error This application can only operate in a linux environment, \
please change the definition of the RTE_TARGET environment variable)
endif

CFLAGS += -O3
CFLAGS += -DALLOW_EXPERIMENTAL_API
CFLAGS += $(WERROR_FLAGS)

include $(RTE_SDK)/mk/rte.extapp.mk
endif
