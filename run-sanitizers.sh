#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SANITIZER_BUILD_DIR="${SANITIZER_BUILD_DIR:-build-sanitizers}"
SANITIZERS="${SANITIZERS:-address,undefined}"
CC_BIN="${CC:-cc}"
JOBS="${JOBS:-$(nproc)}"

if [[ ! "$SANITIZER_BUILD_DIR" =~ ^build-[A-Za-z0-9._-]+$ ]]; then
    printf 'error: SANITIZER_BUILD_DIR must be one relative build-* directory name\n' >&2
    exit 2
fi
if [[ ! "$SANITIZERS" =~ ^[a-z]+(,[a-z]+)*$ ]]; then
    printf 'error: SANITIZERS must be a comma-separated sanitizer list\n' >&2
    exit 2
fi
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    printf 'error: JOBS must be a positive integer\n' >&2
    exit 2
fi
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    printf 'error: compiler not found: %s\n' "$CC_BIN" >&2
    exit 2
fi
if ! pkg-config --exists libdpdk; then
    printf 'error: pkg-config cannot find libdpdk\n' >&2
    exit 2
fi

# DPDK intentionally keeps process-global allocations alive until EAL teardown.
# LeakSanitizer is disabled here so the gate focuses on UAF, double-free, OOB,
# and undefined behaviour. Pool/socket balance needs a separate lifecycle check.
export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:halt_on_error=1:detect_leaks=0:strict_string_checks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"

make_args=(
    "CC=$CC_BIN"
    "BUILD_DIR=$SANITIZER_BUILD_DIR"
    "SANITIZERS=$SANITIZERS"
)
consumer_args=(
    "${make_args[@]}"
    "STACK_BUILD_DIR=$SANITIZER_BUILD_DIR"
)

cd "$ROOT_DIR"

printf '==> Cleaning isolated sanitizer artifacts (%s)\n' "$SANITIZER_BUILD_DIR"
make -C pro-stack "${make_args[@]}" clean
make -C test "${consumer_args[@]}" clean
make -C traffic-gen "${consumer_args[@]}" clean
make -C apps/stack-demo "${consumer_args[@]}" clean

printf '==> Building pro-stack with -fsanitize=%s\n' "$SANITIZERS"
make -C pro-stack -j"$JOBS" "${make_args[@]}" library

printf '==> Building and running the complete sanitizer test suite\n'
make -C test -j"$JOBS" "${consumer_args[@]}" test

printf '==> Verifying sanitizer linkage for traffic-gen and stack-demo\n'
make -C traffic-gen -j"$JOBS" "${consumer_args[@]}" all
make -C apps/stack-demo -j"$JOBS" "${consumer_args[@]}" all

printf '==> ASan/UBSan checks passed\n'
printf '    artifacts: %s/{pro-stack,test,traffic-gen,apps/stack-demo}/%s\n' \
    "$ROOT_DIR" "$SANITIZER_BUILD_DIR"
