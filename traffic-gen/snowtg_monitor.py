"""Local host sampler compatible with the original acceptance_monitor JSONL."""
import argparse
import json
import math
import os
from pathlib import Path
import socket
import sys
import time


def sample(process_names=('traffic-gen', 'nginx', 'dnsmasq', 'redis-server')):
    row = dict(wall_time_ns=time.time_ns(), monotonic_ns=time.monotonic_ns(), host=socket.gethostname(),
               ticks_per_second=os.sysconf('SC_CLK_TCK'), boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip())
    for name in ('stat', 'meminfo', 'net/dev', 'net/sockstat', 'net/snmp', 'net/netstat'):
        row[name] = Path('/proc', name).read_text()
    row['processes'] = {}
    for proc in Path('/proc').glob('[0-9]*'):
        try:
            name = (proc / 'comm').read_text().strip()
            if name in process_names:
                row['processes'][proc.name] = dict(name=name, stat=(proc / 'stat').read_text(),
                                                  status=(proc / 'status').read_text())
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return row


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--interval', type=float, default=5)
    parser.add_argument('--samples', type=int, default=800)
    parser.add_argument('--output', type=Path, help='new JSONL file (default stdout)')
    parser.add_argument('--process', action='append', help='process comm name; repeat to select several')
    args = parser.parse_args(argv)
    if not math.isfinite(args.interval) or args.interval <= 0 or args.samples <= 0:
        parser.error('interval and samples must be positive and finite')
    try:
        output = args.output.open('x') if args.output else sys.stdout
    except OSError as error:
        print('snowtg monitor: ' + str(error), file=sys.stderr)
        return 1
    try:
        for index in range(args.samples):
            print(json.dumps(sample(args.process) if args.process else sample()), file=output, flush=True)
            if index + 1 < args.samples:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0
    except OSError as error:
        print('snowtg monitor: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if args.output:
            output.close()
    return 0
