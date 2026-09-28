#!/usr/bin/env python3
"""Native two-worker mixed Redis/HTTP/DNS compilation, timeouts and reporting.

net_null deliberately provides no responses; this is not live-server acceptance.
"""
from copy import deepcopy
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'traffic-gen'))
from snowtg import assertion
import snowtg_results as results

cpus = sorted(c for c in os.sched_getaffinity(0) if c < 128)[:3]
assert len(cpus) == 3, 'requires three CPUs'
plan = json.loads((ROOT / 'traffic-gen/scenarios/test/redis-mixed.json').read_text())
plan.update(duration_sec=1, target_cps=10, max_concurrency=16)
plan['assertions'] = [assertion('error_rate', '==', 1, protocol='redis'),
                      assertion('error_redis_error', '==', 0),
                      assertion('drained_live_sockets', '==', 0)]
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    source = tmp / 'scenario.json'
    source.write_text(json.dumps(plan))
    binary = os.environ.get('SNOWTG_TEST_BINARY', str(ROOT / 'traffic-gen/build/traffic-gen'))
    cmd = [sys.executable, str(ROOT / 'traffic-gen/snowtg.py'), 'run', '--binary', binary,
           '--output', str(tmp / 'run'), str(source), '--', '-l', ','.join(map(str, cpus)),
           '--main-lcore', str(cpus[2]), '-m', '256', '--no-huge', '--no-pci',
           '--vdev=net_null0', '--file-prefix=redis-' + str(os.getpid()), '--',
           '--workers', '2', '--local-ip', '198.18.0.1']
    done = subprocess.run(cmd, capture_output=True, text=True, timeout=45)
    assert done.returncode == 0, (done.stdout, done.stderr, (tmp / 'run/result.json').read_text())
    result = json.loads((tmp / 'run/result.json').read_text())
    assert result['valid'] and result['summary']['failed'] == 10
    assert result['summary']['planned'] == 10 and result['summary']['start_failed'] == 0
    assert {g['protocol'] for g in result['groups']} == {'redis', 'http', 'dns'}
    assert sum(g['failed'] for g in result['groups'] if g['protocol'] == 'redis') == 6
    assert sum(result['error_reasons'].values()) == 10
    assert result['final_counters']['http_success_total'] == 0
    assert result['resources']['status'] == 'passed'
    assert len(result['resources']['workers']) == 2
    assert all(m['final']['current'] == 0 for w in result['resources']['workers']
               for m in w['metrics'].values())
    assert 'redis' in (tmp / 'run/report.html').read_text()
    assert results.compare(result, result)['recommendation'] == 'inconclusive'

    # Simulate a pre-Redis HTTP/DNS CSV; the additive field may be absent there.
    path = tmp / 'run/workers.csv'
    with path.open() as stream:
        reader = csv.DictReader(stream)
        fields = [f for f in reader.fieldnames if f != 'error_redis_error']
        rows = [{k: v for k, v in row.items() if k in fields} for row in reader]
    with path.open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerows(rows)
    old = deepcopy(result)
    for cls in old['scenario']['classes']:
        if 'redis' in cls:
            cls.pop('redis')
            cls['http'] = dict(method='GET', path='/', keepalive=True)
    latency_path = tmp / 'run/latency.csv'
    latency_text = latency_path.read_text()
    latency_path.write_text(latency_text.replace(',redis,', ',http,'))
    old['invalid_reasons'] = []
    results.collect(old, tmp / 'run')
    assert not old['invalid_reasons'], old['invalid_reasons']
    assert 'error_redis_error' not in old['error_reasons']
    latency_path.write_text(latency_text)
    missing = deepcopy(result)
    missing['invalid_reasons'] = []
    results.collect(missing, tmp / 'run')
    assert any('error reasons' in s for s in missing['invalid_reasons'])
print('PASS: native mixed Redis classes, two workers, failure accounting, clean drain and legacy CSV')
