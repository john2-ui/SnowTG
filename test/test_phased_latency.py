#!/usr/bin/env python3
"""Two-owner CLI check: bounded open arrivals, cross-phase timeouts and CSV bins."""
from collections import Counter
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', nargs='?', default=str(
    Path(__file__).resolve().parents[1] / 'traffic-gen/build/traffic-gen'))
frontend = parser.add_mutually_exclusive_group()
frontend.add_argument('--python', action='store_true', help='exercise Python scenario launcher')
frontend.add_argument('--lua', action='store_true', help='exercise Lua scenario launcher')
options = parser.parse_args()
cpus = sorted(c for c in os.sched_getaffinity(0) if c < 128)
if len(cpus) < 3:
    print('SKIP: requires three CPUs')
    raise SystemExit(0)
binary = Path(options.binary).resolve()
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    scenario = dict(name='phase-latency-test', load_model='open', max_concurrency=4,
        report_interval_sec=1, phases=[
            dict(name='warm,up', duration_sec=1, target_cps=20),
            dict(name='spike', duration_sec=1, start_cps=20, target_cps=40),
            dict(name='cooldown', duration_sec=1, target_cps=0)],
        classes=[dict(name='dns,one', weight=1, transport='udp',
            peer=dict(ip='198.18.0.2', port=53), dns=dict(qname='test.invalid', qtype='A'))])
    (root / 'scenario.json').write_text(json.dumps(scenario))
    script = root / 'scenario.py'
    script.write_text('''from snowtg import scenario, phase, dns
plan = scenario('phase-latency-test', concurrency=4,
    phases=[phase('warm,up', 1, 20), phase('spike', 1, 40, start=20),
            phase('cooldown', 1, 0)],
    classes=[dns('dns,one', '198.18.0.2', 'test.invalid')])
''')
    if options.lua:
        script = root / 'scenario.lua'
        script.write_text('''local tg = require("snowtg")
plan = tg.scenario('phase-latency-test', {concurrency=4,
    phases={tg.phase('warm,up', 1, 20), tg.phase('spike', 1, 40, {start=20}),
            tg.phase('cooldown', 1, 0)},
    classes={tg.dns('dns,one', '198.18.0.2', 'test.invalid')}})
''')
    scripted = options.python or options.lua
    launcher = Path(__file__).resolve().parents[1] / 'traffic-gen/snowtg.py'
    prefix = [sys.executable, str(launcher), '--binary', str(binary), str(script), '--'] \
             if scripted else [str(binary)]
    args = ['-l', ','.join(map(str, cpus[:3])),
        '--main-lcore', str(cpus[2]), '-m', '256', '--no-huge', '--no-pci',
        '--vdev=net_null0', '--file-prefix=phases-%d' % os.getpid(), '--',
        '--workers', '2', '--local-ip', '198.18.0.1', '--stats-csv', str(root/'stats.csv'),
        '--latency-csv', str(root/'latency.csv')]
    result = subprocess.run(prefix + args + ([] if scripted else [str(root/'scenario.json')]),
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    assert result.returncode == 0, result.stdout
    rows = list(csv.DictReader((root/'latency.csv').open()))
    stats = list(csv.DictReader((root/'stats.csv').open()))
    for row in rows:
        assert None not in row, row
        samples = int(row['samples'])
        bins = [tuple(map(int, pair.split(':'))) for pair in row['buckets'].split(';') if pair]
        assert sum(count for _, count in bins) == samples, row
        quantiles = [int(row[q]) for q in ('p50_us','p90_us','p95_us','p99_us','p999_us','max_us')]
        assert quantiles == sorted(quantiles), row
        if row['metric'] != 'drain':
            assert int(row['planned']) == int(row['attempted']) + int(row['skipped']), row
            assert int(row['attempted']) == int(row['admitted']) + int(row['start_failed']), row
            assert int(row['admitted']) == int(row['success']) + int(row['failed']), row
    for row in rows:
        if row['scope'] not in ('class', 'protocol'):
            continue
        related = [r for r in rows if r['scope']=='worker' and
                   r['load_phase']==row['load_phase'] and r['metric']==row['metric'] and
                   r['protocol']==row['protocol'] and
                   (row['scope']=='protocol' or r['class']==row['class'])]
        bins = Counter()
        for r in related:
            bins.update({int(k):int(v) for k,v in
                         (pair.split(':') for pair in r['buckets'].split(';') if pair)})
        merged = {int(k):int(v) for k,v in
                  (pair.split(':') for pair in row['buckets'].split(';') if pair)}
        assert dict(bins)==merged, row
    classes = [r for r in rows if r['scope']=='class' and r['metric']=='complete']
    assert [int(r['planned']) for r in classes] == [20,30,0], classes
    warmup = classes[0]
    assert int(warmup['admitted']) == 4 and int(warmup['failed']) == 4, warmup
    assert int(warmup['max_us']) >= 4_900_000, warmup
    assert int(classes[1]['skipped']) == 30, classes[1]
    assert int(classes[1]['samples']) == 0, classes[1]
    final = next(r for r in stats if r['scope']=='aggregate' and r['phase']=='final')
    assert int(final['arrivals_planned']) == 50 and int(final['arrivals_skipped']) == 46, final
    assert int(final['live_sockets']) == 0 and int(final['active']) == 0, final
    drain = next(r for r in rows if r['scope']=='run' and r['metric']=='drain')
    assert int(drain['samples']) == 1 and int(drain['max_us']) >= 1_900_000, drain
    if scripted:
        # Script frontends must retain the C schema checks, including fields
        # supplied directly in a dictionary instead of through the helpers.
        script.write_text(script.read_text() + "\nplan['load_model'] = 'closed'\n")
        result = subprocess.run(prefix + args, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=10)
        assert result.returncode != 0 and 'scenario load failed' in result.stdout, result.stdout
    print('PASS: 50 planned = 4 timed-out + 46 skipped; two-owner bins, phase attribution and drain verified')
