APP := stack-demo
BUILD_DIR ?= build
STACK_BUILD_DIR ?= $(BUILD_DIR)
SANITIZERS ?=
STACK_LIB := ../../pro-stack/$(STACK_BUILD_DIR)/libpro-stack.a
SRCS := main.c ../tcp-echo/tcp_echo.c ../udp-echo/udp_echo.c

PKGCONF := pkg-config --define-prefix
CFLAGS += -O3 -I../../pro-stack $(shell $(PKGCONF) --cflags libdpdk)
CFLAGS += -DALLOW_EXPERIMENTAL_API
LDFLAGS := $(shell $(PKGCONF) --static --libs libdpdk)
ifneq ($(strip $(SANITIZERS)),)
CFLAGS += -O1 -g3 -fno-omit-frame-pointer -fno-optimize-sibling-calls
CFLAGS += -fsanitize=$(SANITIZERS)
LDFLAGS += -fsanitize=$(SANITIZERS)
endif

.PHONY: all clean
all: $(BUILD_DIR)/$(APP)

$(STACK_LIB):
	$(MAKE) -C ../../pro-stack library BUILD_DIR=$(STACK_BUILD_DIR) \
		SANITIZERS=$(SANITIZERS)

$(BUILD_DIR)/$(APP): $(SRCS) Makefile $(STACK_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRCS) $(STACK_LIB) -o $@ $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -f $(BUILD_DIR)/$(APP)
	rmdir $(BUILD_DIR) 2>/dev/null || true
