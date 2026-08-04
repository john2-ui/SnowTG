APP := traffic-gen
LLHTTP_DIR := ../third_party/llhttp
SRCS := main_tg.c \
	core/reactor.c \
	core/flow.c \
	core/flow_pool.c \
	core/txn.c \
	proto/http/http_client.c \
	$(LLHTTP_DIR)/src/llhttp.c \
	$(LLHTTP_DIR)/src/api.c \
	$(LLHTTP_DIR)/src/http.c

PKGCONF := pkg-config --define-prefix
DPDK_PC := $(shell $(PKGCONF) --path libdpdk)
CFLAGS += -O3 -I. -Icore -Iproto -I../pro-stack -I$(LLHTTP_DIR)/include \
	$(shell $(PKGCONF) --cflags libdpdk)
CFLAGS += -DALLOW_EXPERIMENTAL_API
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
