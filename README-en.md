# SnowTG

## Project Overview

`SnowTG` is a DPDK-based userspace IPv4 network stack and mixed traffic generator. It uses a single-owner, per-core reactor architecture with lock-free hot paths, provides TCP/UDP socket capabilities, and drives HTTP/DNS load traffic through its own network stack.

中文文档：[`README.md`](README.md)

The repository contains:

- [`pro-stack/`](pro-stack/): a userspace Ethernet/ARP/IPv4/ICMP/TCP/UDP stack with BSD-style APIs and owner-local non-blocking interfaces.
- [`traffic-gen/`](traffic-gen/): a scenario-driven HTTP/1.1 and DNS traffic generator with CPS control, concurrency limits, connection reuse, sharded scheduling, and CSV metrics.
- [`apps/`](apps/): TCP/UDP echo examples and the network-stack runtime entry point.
- [`test/`](test/): regression tests for protocols, owner lifecycle management, scheduling, scenario parsing, and statistics.

See [`docs/TODO.md`](docs/TODO.md) for the architecture and planned work, and [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) for performance results.

## Usage

### Build

Install DPDK and the project dependencies, then configure huge pages and a DPDK-compatible NIC. Each target can be built independently:

```bash
# Network-stack static library: pro-stack/build/libpro-stack.a
make -C pro-stack

# TCP/UDP echo example: apps/stack-demo/build/stack-demo
make -C apps/stack-demo

# Mixed traffic generator: traffic-gen/build/traffic-gen
make -C traffic-gen

# Build and run the test suite
make -C test
```

To check for memory errors and undefined behavior in an isolated build directory, run:

```bash
./run-sanitizers.sh
# Optional: CC=clang JOBS=8 ./run-sanitizers.sh
```

### Run the Example Stack

Bind the target NIC to a DPDK driver when required, then start the example application:

```bash
./bind-dpdk.sh
./apps/stack-demo/build/stack-demo -l 0-2 ...
```

Compile-time settings for the TCP/UDP echo examples and local network identity are defined in [`pro-stack/config.h`](pro-stack/config.h). Common options include `ENABLE_TCP_APP`, `ENABLE_TCP_CLIENT`, `ENABLE_TCP_SERVER`, `ENABLE_UDP_APP`, `ENABLE_ARP`, and `ENABLE_ICMP`.

### Run traffic-gen

Place DPDK EAL arguments before `--`, and traffic-gen arguments plus the scenario path after it:

```bash
./traffic-gen/build/traffic-gen -l 0-1 -- \
  --workers 1 \
  --local-ip 192.168.21.2 \
  --port-id 0 \
  --stats-csv traffic-gen/results.csv \
  traffic-gen/scenarios/test/mix-http-dns.json
```

The complete application syntax is:

```text
traffic-gen [EAL arguments] -- [--workers N] [--socket-id-max N]
            [--stats-csv PATH] [--mtu BYTES]
            [--local-ip IPv4] [--port-id N] [scenario.json]
```

- `--workers`: number of network-stack owner/reactor workers; defaults to `1`.
- `--socket-id-max`: manually sets the socket capacity for each owner; when omitted, the value is calculated from the scenario.
- `--stats-csv`: writes periodic statistics to the specified CSV file.
- `--mtu`: sets the IPv4 MTU.
- `--local-ip`: sets the stack's local IPv4 address; defaults to `192.168.21.2`.
- `--port-id`: selects a DPDK Ethernet port id enumerated by EAL; defaults to `0`.
- `scenario.json`: load-test scenario; examples are available in [`traffic-gen/scenarios/`](traffic-gen/scenarios/).

`--local-ip` and `--port-id` configure the traffic-generator endpoint. The
`peer.ip` and `peer.port` fields in each scenario class continue to identify
the target service and its service port.

### Add an Application-Layer Protocol Plugin

The current plugin mechanism uses source integration and compile-time static
registration; it does not load `.so` files at runtime. A plugin has two parts:
[`tg_proto_ops`](traffic-gen/proto/proto.h) processes request and response
bytes, while [`tg_proto_scenario`](traffic-gen/proto/registry.h) compiles the
protocol object in a scenario into immutable configuration. The implementations
under [`traffic-gen/proto/http/`](traffic-gen/proto/http/) and
[`traffic-gen/proto/dns/`](traffic-gen/proto/dns/) are working references.

For a new protocol named `myproto`, start with this layout:

```text
traffic-gen/proto/myproto/
├── myproto_client.c
├── myproto_client.h
├── myproto_scenario.c
└── myproto_scenario.h
```

#### 1. Implement the Byte-Protocol Contract

Define immutable class configuration and export the operations table from
`myproto_client.h`:

```c
#include "../proto.h"

struct tg_myproto_config {
        /* Own all data; do not reference the temporary scenario JSON buffer. */
        char request_value[128];
};

extern const struct tg_proto_ops tg_myproto_ops;
```

Implement the callbacks and operations table in `myproto_client.c`:

```c
#include "myproto_client.h"
#include "../../core/txn.h"

static int my_config_clone(const void *source, void **destination);
static void my_config_free(void *config);
static int my_init(struct tg_txn *txn);
static int my_build_request(const void *config, uint8_t *buffer,
                            size_t capacity, size_t *length_out);
static enum tg_proto_result my_on_rx(struct tg_txn *txn,
                                     const uint8_t *data, size_t length);
static enum tg_proto_result my_on_eof(struct tg_txn *txn);
static void my_reset(struct tg_txn *txn);

const struct tg_proto_ops tg_myproto_ops = {
    .name = "myproto",
    .config_clone = my_config_clone,
    .config_free = my_config_free,
    .init = my_init,
    .build_request = my_build_request,
    .on_rx = my_on_rx,
    .on_eof = my_on_eof,
    .reset = my_reset,
};
```

Follow these rules when implementing the callbacks:

- `class_config` is read-only on the runtime hot path. If it is non-`NULL`,
  implement both `config_clone` and `config_free` so worker shards can copy and
  destroy it safely.
- `init` may allocate per-transaction parser state in `txn->proto_ctx`; `reset`
  must release that state.
- `build_request` serializes one request into caller-owned storage, checks its
  capacity, and returns the actual length.
- A TCP `on_rx` must accept arbitrarily split or coalesced byte chunks. A UDP
  `on_rx` receives one complete datagram per call. Plugins parse application
  bytes only and must not call `owner_io_*` or manage sockets directly.
- `on_rx` and `on_eof` return `TG_PROTO_MORE`, `TG_PROTO_COMPLETE`, or
  `TG_PROTO_FAILED`. Set `txn->connection_reusable` to `true` only when the
  completed response permits TCP connection reuse.
- Implement the optional `on_tx_accepted` callback if the parser needs to track
  request bytes accepted by the transport.

#### 2. Compile the Scenario Protocol Object

Implement a compiler like `tg_http_scenario_compile()`. It must:

1. Use the helpers in [`scenario_json.h`](traffic-gen/core/scenario_json.h) to
   validate the `myproto` object and accept only declared fields.
2. Allocate and fully copy a `tg_myproto_config`; never retain JSON token or
   text pointers.
3. Set `class_plan->proto` and `class_plan->proto_config`.
4. Call `build_request` to precompile `class_plan->request_template` and set
   `request_template_len`. The request must fit within
   `TG_PLAN_REQUEST_TEMPLATE_CAP`.
5. On failure, release allocated configuration, clear ownership fields, and set
   an appropriate `errno`.

Include the new headers in [`registry.c`](traffic-gen/proto/registry.c), then
add a descriptor to its static table:

```c
{
    .schema_key = "myproto",
    .ops = &tg_myproto_ops,
    .transport = TG_TRANSPORT_TCP, /* or TG_TRANSPORT_UDP */
    .compile = tg_myproto_scenario_compile,
},
```

Each class must contain exactly one registered protocol key, and its
`transport` must match the registered descriptor.

#### 3. Add It to the Build and Use It

Add `myproto_client.c` and `myproto_scenario.c` to `SRCS` in
[`traffic-gen/Makefile`](traffic-gen/Makefile). The protocol can then be used in
a scenario:

```json
{
  "name": "myproto-example",
  "duration_sec": 30,
  "max_concurrency": 100,
  "target_cps": 20,
  "classes": [
    {
      "name": "my-request",
      "weight": 1,
      "transport": "tcp",
      "peer": { "ip": "192.168.21.106", "port": 9000 },
      "myproto": { "request_value": "hello" }
    }
  ]
}
```

Add protocol unit tests for request construction, fragmented responses,
invalid responses, EOF, and reset. Extend the scenario tests to cover valid
configuration, unknown fields, a mismatched transport, and configuration
clone/free. Then run:

```bash
make -C traffic-gen
make -C test
```

### Logging and Troubleshooting

The default build only emits TCP/ARP warnings and errors to avoid excessive per-packet logs at high CPS. Clean previous build artifacts before changing logging variables:

```bash
make -C pro-stack clean

# TCP lifecycle and retransmission logs without per-packet output
make -C pro-stack LOG_LEVEL=LOG_LVL_DEBUG \
  TCP_LOG_INFO_ENABLED=1 TCP_LOG_PACKETS=0

# ARP debug logs
make -C pro-stack LOG_LEVEL=LOG_LVL_DEBUG ARP_LOG_ENABLED=1

# TCP per-packet logs
make -C pro-stack LOG_LEVEL=LOG_LVL_TRACE \
  TCP_LOG_INFO_ENABLED=1 TCP_LOG_DEBUG_ENABLED=1 \
  TCP_LOG_TRACE_ENABLED=1 TCP_LOG_PACKETS=1
```

Set `NO_COLOR=1` or `LOG_COLOR=never` to disable colored log output. See [`docs/DEBUG.md`](docs/DEBUG.md) and [`docs/ERROR.md`](docs/ERROR.md) for more troubleshooting information.
