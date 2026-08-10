# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2010-2014 Intel Corporation

# TCP INFO and TRACE logs are disabled by default for traffic generation.
# Enable the required category explicitly while debugging, for example:
#   make LOG_LEVEL=LOG_LVL_TRACE TCP_LOG_INFO_ENABLED=1 \
#       TCP_LOG_DEBUG_ENABLED=1 TCP_LOG_TRACE_ENABLED=1 TCP_LOG_PACKETS=1
# INFO keeps normal traffic-generator statistics visible without per-flow
# owner and transport DEBUG output. Explicit debugging commands override it.
LOG_LEVEL ?= LOG_LVL_INFO
ARP_LOG_ENABLED ?= 0
TCP_LOG_PACKETS ?= 0
TCP_LOG_INFO_ENABLED ?= 0
TCP_LOG_DEBUG_ENABLED ?= 0
TCP_LOG_TRACE_ENABLED ?= 0

STACK_SRCS := net_context.c ring.c arp.c udp.c icmp.c port.c rx_dispatch.c socket.c \
	socket_owner.c pkt_frame.c tcp.c tcp_options.c tcp_rtt.c rbtree.c \
	owner_io.c stack_runtime.c tcp_memory.c udp_memory.c
STACK_OBJS := $(patsubst %.c,build/lib/%.o,$(STACK_SRCS))
STACK_DEPS := $(STACK_OBJS:.o=.d)

-include $(STACK_DEPS)

# Build using pkg-config variables if possible
ifeq ($(shell pkg-config --exists libdpdk && echo 0),0)

all: library
.PHONY: library
library: build/libpro-stack.a

PKGCONF=pkg-config --define-prefix

PC_FILE := $(shell $(PKGCONF) --path libdpdk)
CFLAGS += -O3 $(shell $(PKGCONF) --cflags libdpdk)
CFLAGS += -DALLOW_EXPERIMENTAL_API
CFLAGS += -DLOG_LEVEL=$(LOG_LEVEL) -DARP_LOG_ENABLED=$(ARP_LOG_ENABLED)
CFLAGS += -DTCP_LOG_PACKETS=$(TCP_LOG_PACKETS)
CFLAGS += -DTCP_LOG_INFO_ENABLED=$(TCP_LOG_INFO_ENABLED)
CFLAGS += -DTCP_LOG_DEBUG_ENABLED=$(TCP_LOG_DEBUG_ENABLED)
CFLAGS += -DTCP_LOG_TRACE_ENABLED=$(TCP_LOG_TRACE_ENABLED)
LDFLAGS_STATIC = $(shell $(PKGCONF) --static --libs libdpdk)

build/libpro-stack.a: $(STACK_OBJS) | build
	ar rcs $@ $(STACK_OBJS)

build/lib/%.o: %.c Makefile $(PC_FILE) | build/lib
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

build:
	@mkdir -p $@

build/lib:
	@mkdir -p $@

.PHONY: clean
clean:
	rm -f build/libpro-stack.a
	rm -rf build/lib
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
CFLAGS += -DLOG_LEVEL=$(LOG_LEVEL) -DARP_LOG_ENABLED=$(ARP_LOG_ENABLED)
CFLAGS += -DTCP_LOG_PACKETS=$(TCP_LOG_PACKETS)
CFLAGS += -DTCP_LOG_INFO_ENABLED=$(TCP_LOG_INFO_ENABLED)
CFLAGS += -DTCP_LOG_DEBUG_ENABLED=$(TCP_LOG_DEBUG_ENABLED)
CFLAGS += -DTCP_LOG_TRACE_ENABLED=$(TCP_LOG_TRACE_ENABLED)
CFLAGS += $(WERROR_FLAGS)

include $(RTE_SDK)/mk/rte.extapp.mk
endif
