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
  --stats-csv traffic-gen/results.csv \
  traffic-gen/scenarios/test/mix-http-dns.json
```

The complete application syntax is:

```text
traffic-gen [EAL arguments] -- [--workers N] [--socket-id-max N]
            [--stats-csv PATH] [--mtu BYTES] [scenario.json]
```

- `--workers`: number of network-stack owner/reactor workers; defaults to `1`.
- `--socket-id-max`: manually sets the socket capacity for each owner; when omitted, the value is calculated from the scenario.
- `--stats-csv`: writes periodic statistics to the specified CSV file.
- `--mtu`: sets the IPv4 MTU.
- `scenario.json`: load-test scenario; examples are available in [`traffic-gen/scenarios/`](traffic-gen/scenarios/).

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
