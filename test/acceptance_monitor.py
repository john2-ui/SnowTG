#!/usr/bin/env python3
"""Sample host/process counters as JSONL beside a hardware acceptance run.

Counters are cumulative; compare deltas between samples. wall_time_ns aligns
hosts only to the accuracy of their system clocks. No clock is adjusted here.
"""
import argparse
import json
from pathlib import Path
import time


def sample():
    row = {'wall_time_ns': time.time_ns(), 'monotonic_ns': time.monotonic_ns()}
    for name in ('stat', 'meminfo', 'net/dev', 'net/sockstat', 'net/snmp', 'net/netstat'):
        row[name] = Path('/proc', name).read_text()
    row['processes'] = {}
    for proc in Path('/proc').glob('[0-9]*'):
        try:
            name = (proc / 'comm').read_text().strip()
            if name in ('traffic-gen', 'nginx', 'dnsmasq'):
                row['processes'][proc.name] = {'name': name, 'stat': (proc / 'stat').read_text(),
                                               'status': (proc / 'status').read_text()}
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return row


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--interval', type=float, default=5)
    parser.add_argument('--samples', type=int, default=800)
    args = parser.parse_args()
    if args.interval <= 0 or args.samples <= 0:
        parser.error('interval and samples must be positive')
    for index in range(args.samples):
        print(json.dumps(sample()), flush=True)
        if index + 1 < args.samples:
            time.sleep(args.interval)
