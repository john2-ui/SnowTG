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

### Build and Smoke-Test with Docker

With Docker Engine installed, run from the repository root (at least two
available CPUs are required):

```bash
docker build -t snowtg . && docker run --rm --network none snowtg
```

The Ubuntu 26.04 image downloads and verifies the SHA-256 of DPDK 26.07, builds it
from source, then builds `traffic-gen` and `stack-demo` and runs `make -C test`.
Any failure stops the build. This DPDK version provides the duplicate IPv4
fragment behavior required by the regression suite. `.dockerignore` excludes
host binaries and benchmark artifacts. The image uses shared DPDK libraries and
includes null, pcap, AF_PACKET, VMXNET3, virtio, and Intel e1000/igc PMDs, covering
the project's VM and NUC NICs. Use
`--build-arg 'DPDK_DRIVERS=bus/*,common/*,mempool/*,net/*'` for all buildable network
drivers; drivers such as mlx5 also need their development packages added to the
Dockerfile. Build tools and tests remain available for repeat checks.
The default build uses four jobs; override with
`docker build --build-arg JOBS=8 -t snowtg .`. DPDK uses a generic CPU configuration
so the image does not inherit the build host's specific instruction set. The
first build takes longer; subsequent builds can reuse the dependency cache.

The default command reuses `test/test_lcore_layout.py`: it selects two available
CPUs, runs a one-second DNS scenario with the `net_null` virtual NIC and 256 MiB
of ordinary memory, and checks the exit status and final CSV record. It prints
`PASS` and exits; no hugepages, privileges, or physical NIC are needed. The virtual
NIC has no DNS peer, so this checks startup and shutdown, not successful requests
or throughput. With fewer than two available CPUs whose IDs are below 128,
`SKIP` means the startup check did not run.

```bash
docker run --rm --network none snowtg make -C test
docker run --rm snowtg traffic-gen --help
```

Real traffic requires a Linux host with hugepages, IOMMU, and a dedicated test NIC
bound to `vfio-pci` using `bind-dpdk.sh`. See the
[DPDK container guide](https://doc.dpdk.org/guides/linux_gsg/build_sample_apps.html#running-an-application-in-a-container).
Docker does not configure host drivers or unbind NICs. The example below assumes
hugepages at `/dev/hugepages`, PCI address `0000:64:00.0`, and IOMMU group `17`.
Replace those values, CPU IDs and local IP, and update the scenario's `peer.ip`
and `peer.port`. Find the group with
`readlink /sys/bus/pci/devices/0000:64:00.0/iommu_group`.

```bash
mkdir -p docker-results
docker run --rm --init --network none \
  --device /dev/vfio/vfio --device /dev/vfio/17 \
  --cap-add IPC_LOCK --ulimit memlock=-1:-1 \
  --mount type=bind,src=/dev/hugepages,dst=/dev/hugepages \
  --mount "type=bind,src=$(pwd)/traffic-gen/scenarios,dst=/scenarios,readonly" \
  --mount "type=bind,src=$(pwd)/docker-results,dst=/results" \
  snowtg traffic-gen -l 0-1 -a 0000:64:00.0 \
  --huge-dir /dev/hugepages --file-prefix snowtg -- \
  --workers 1 --local-ip 192.168.21.2 --port-id 0 \
  --stats-csv /results/stats.csv /scenarios/test/mix-http-dns.json
```

Traffic uses the VFIO NIC directly, without Docker port mapping. Results persist
in `docker-results/stats.csv`. Concurrent instances need separate NICs, CPUs, and
file prefixes. UIO device passthrough needs a different configuration; this
example covers VFIO only. Override the image command with `stack-demo` and its
EAL arguments to run the echo application instead.

### Run the Example Stack

Bind the target NIC to a DPDK driver when required, then start the example application:

```bash
./bind-dpdk.sh --status
./bind-dpdk.sh --dry-run enp100s0
./bind-dpdk.sh enp100s0                    # Default: vfio-pci, requires IOMMU
./bind-dpdk.sh --driver uio_pci_generic ens160  # Explicit lab VM alternative
./apps/stack-demo/build/stack-demo -l 0-2 ...
```

The script accepts an interface or PCI address; run it as your login user and it
uses `sudo` for changes. Locate `dpdk-devbind.py` through `DPDK_DEVBIND`, `PATH`,
`DPDK_DIR/usertools`, `../dpdk/usertools`, or `--devbind PATH`. `igb_uio` is also
supported when separately installed. UIO support depends on the device and does
not provide VFIO's IOMMU isolation; there is no automatic driver fallback.
`--force` permits an addressed/routed dedicated test port, but cannot override
protection of the current SSH return path. Use independent management first.
Restore the kernel driver with, for example,
`./bind-dpdk.sh --driver igc 0000:64:00.0`; restore IP addresses, routes and NIC
settings separately. No arguments only show status; no application is started.

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
            [--stats-csv PATH] [--latency-csv PATH] [--mtu BYTES]
            [--dataplane-csv PATH] [--metrics-sample N]
            [--rx-mode main|worker|auto] [--tx-mode main|worker|auto]
            [--local-ip IPv4] [--port-id N] [scenario.json]
```

- `--workers`: number of network-stack owner/reactor workers; defaults to `1`.
- `--socket-id-max`: per-owner socket capacity, allocated at startup as `max(16384, 2 * ceil(global concurrency / active_shards))` by default. An explicit value may lower this default but must cover `max(4096, 2 * ceil(global concurrency / active_shards))`. Live tables are not resized.
- `--stats-csv`: writes periodic statistics to the specified CSV file.
- `--latency-csv`: per-phase, protocol and class histograms with P50/P90/P95/P99/P99.9 for scheduling, connection, first response, completion and drain latency.
- `--dataplane-csv`: writes Main timings, packet counters and NIC counter deltas to a separate CSV file.
- `--metrics-sample`: samples added timings every N loops; defaults to `1024`. Zero disables timing; omitting `--dataplane-csv` also disables it. Use sampled packet counts when calculating time per packet.
- `--rx-mode`: defaults to `auto`, selecting worker RX when RSS configuration and independent queues are available, or with a single worker. Otherwise Main dispatches packets. Explicit `worker` fails if unsupported. Misrouted packets return to their socket owner; worker 0 handles ARP replies and fragment reassembly.
- `--tx-mode`: defaults to `auto`, assigning each worker a dedicated TX queue when available and falling back to Main otherwise. Explicit `worker` requires enough queues. Each worker sends up to four bursts per turn and drains its ring on exit.

- `--mtu`: sets the IPv4 MTU.
- `--local-ip`: sets the stack's local IPv4 address; defaults to `192.168.21.2`.
- `--port-id`: selects a DPDK Ethernet port id enumerated by EAL; defaults to `0`.
- `scenario.json`: load-test scenario; examples are available in [`traffic-gen/scenarios/`](traffic-gen/scenarios/).

Worker CSV reports include NIC RX/TX and handoff counters. Successful RSS configuration
does not prove actual packet distribution: this VM's vmxnet3 backend delivers all
traffic to RXQ0. Use `--rx-mode main --tx-mode worker` for this setup; see the
[performance records](docs/PERFORMANCE.md) for measurements.

`--local-ip` and `--port-id` configure the traffic-generator endpoint. The
`peer.ip` and `peer.port` fields in each scenario class continue to identify
the target service and its service port.

### Scripted Scenarios, SLOs and Reports

`python3 traffic-gen/snowtg.py` accepts `.json`, `.py` and `.lua`. Requires Python 3.8+;
Lua scenarios also need Lua 5.3/5.4 (`SNOWTG_LUA` can select the interpreter).
Scripts generate configuration at startup and do not participate in packet processing.
Start with the [Python](traffic-gen/scenarios/test/acceptance-http-dns.py) or
[Lua](traffic-gen/scenarios/test/acceptance-http-dns.lua) example:

- `scenario` sets global concurrency; `http` / `dns` define weighted traffic classes.
- `phase` defines warmup, ramp, steady, spike and cooldown stages; `start` enables a linear rate change. The arrival model is open.
- `assertion` checks success rate, latency quantiles, allocation failures and drain, with phase/class selectors where applicable.
  `success_rate` uses planned arrivals as its denominator; `latency_ms` defaults to successful transaction completion latency.

Run from the project root. This NUC example requires matching CPU IDs, PCI address and local IP.
The example targets `192.168.10.234:8888/1053`; set `SNOWTG_PEER` to change the target IP
and `SNOWTG_SERVICE_VERSION` to record the tested service version.

```bash
python3 traffic-gen/snowtg.py run --output debug/run1 \
  traffic-gen/scenarios/test/acceptance-http-dns.lua -- \
  -l 4,0,2 --main-lcore 4 -a 0000:64:00.0 -m 512 -- \
  --workers 2 --local-ip 192.168.10.86

# Repeat with output directory debug/run2, then compare
python3 traffic-gen/snowtg.py compare debug/run1/result.json debug/run2/result.json

# Generate an offline report including baseline differences
python3 traffic-gen/snowtg.py report debug/run2/result.json \
  --baseline debug/run1/result.json --output debug/comparison.html
```

`run` saves CSVs and logs, records configuration, environment/build metadata and SLO results in `result.json`, and generates `report.html`.
Use a new output directory; CSV paths are managed automatically. Exit codes: `0` passes acceptance,
`2` fails a critical SLO, `1` is an invalid run. Add `--baseline PATH` before the scenario path to include
a baseline during a run; export configuration only with `--emit-json PATH` (without `run`).
Comparison checks workload/environment compatibility; maximum sustainable load remains unmeasured.
Generated results under `debug/` are ignored by Git.

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
