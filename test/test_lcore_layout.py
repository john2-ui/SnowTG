#!/usr/bin/env python3
"""CLI regression: workers below Main's lcore ID must remain selectable."""
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

cpus = sorted(cpu for cpu in os.sched_getaffinity(0) if cpu < 128)
if len(cpus) < 2:
    print('SKIP: requires two available CPUs')
    raise SystemExit(0)
worker, main = cpus[0], cpus[1]
binary = Path(sys.argv[1] if len(sys.argv) > 1 else
              Path(__file__).resolve().parents[1] / 'traffic-gen/build/traffic-gen').resolve()
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    scenario = root / 'scenario.json'
    stats = root / 'stats.csv'
    scenario.write_text(json.dumps(dict(
        name='lcore-layout', duration_sec=1, max_concurrency=1, target_cps=1,
        report_interval_sec=1, classes=[dict(name='dns', weight=1, transport='udp',
            peer=dict(ip='198.18.0.2', port=53), dns=dict(qname='test.invalid', qtype='A'))])))
    result = subprocess.run([
        str(binary), '-l', '%d,%d' % (worker, main), '--main-lcore', str(main),
        '-m', '256', '--no-huge', '--no-pci', '--vdev=net_null0',
        '--file-prefix=lcore-layout-%d' % os.getpid(), '--', '--workers', '1',
        '--local-ip', '198.18.0.1', '--stats-csv', str(stats), str(scenario)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    assert result.returncode == 0, result.stdout
    with stats.open() as stream:
        rows = list(csv.DictReader(stream))
    finals = [row for row in rows if row['scope'] == 'worker' and row['phase'] == 'final']
    assert len(finals) == 1 and int(finals[0]['lcore']) == worker, finals
    print('PASS: Main=%d, worker=%d; no physical NIC used' % (main, worker))
