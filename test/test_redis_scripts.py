#!/usr/bin/env python3
"""Redis helpers, equivalent examples and protocol-scoped SLO/report behavior."""
from copy import deepcopy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'traffic-gen'))
from snowtg import redis, scenario, assertion
import snowtg_results as results

example = ROOT / 'traffic-gen/scenarios/test/redis-mixed.json'
expected = json.loads(example.read_text())
with tempfile.TemporaryDirectory() as tmp:
    for suffix in ('py', 'lua'):
        if suffix == 'lua' and not (os.environ.get('SNOWTG_LUA') or any(
                shutil.which(x) for x in ('lua', 'lua5.4', 'lua5.3'))):
            print('SKIP: Lua interpreter unavailable')
            continue
        output = Path(tmp) / (suffix + '.json')
        subprocess.run([sys.executable, str(ROOT / 'traffic-gen/snowtg.py'),
                        '--emit-json', str(output), str(example.with_suffix('.' + suffix))], check=True)
        assert json.loads(output.read_text()) == expected

c = redis('set', '198.18.0.2', command='SET', key='', value='雪\r\n', keepalive=False)
assert c['peer']['port'] == 6379 and c['redis']['key'] == ''
assert c['redis']['value'] == '雪\r\n' and c['redis']['keepalive'] is False
plan = scenario('redis', duration=1, cps=1, classes=[c], assertions=[
    assertion('success_rate', '==', 1, protocol='redis'),
    assertion('success_rps', '>=', 1, protocol='redis', class_name='set'),
    assertion('latency_ms', '<', 10, protocol='redis', quantile=.99),
    assertion('error_redis_error', '==', 0)])
results.validate_assertions(plan)
bad = deepcopy(plan)
bad['assertions'][0]['protocol'] = 'dns'
try:
    results.validate_assertions(bad)
    raise AssertionError('Redis was incorrectly identified as DNS')
except ValueError:
    pass

hist = results.distribution(dict(samples=2, max_us=1, buckets=[[1, 2]]))
result = dict(schema_version=2, scenario=plan, workload_sha256='redis', valid=True,
    status='passed', environment={'cpu_model': 'unit', 'dataplane': {}},
    summary=dict(duration_sec=1, success_rps=2, p95_ms=.001, p99_ms=.001, error_rate=0),
    groups=[dict(phase='steady', **{'class': 'set'}, protocol='redis', planned=2,
                 attempted=2, skipped=0, admitted=2, success=2, failed=0, start_failed=0,
                 latency={m: hist for m in results.LATENCIES})],
    error_reasons={k: 0 for k in results.ERROR_REASONS}, errors={}, resource_peaks={}, invalid_reasons=[])
result['assertions'] = results.evaluate(result)
assert all(a['passed'] for a in result['assertions'])
assert results.compare(result, result)['recommendation'] == 'accept_within_tested_scope'
candidate = deepcopy(result)
candidate['errors']['error_redis_error'] = 1
assert results.compare(result, candidate)['new_error_types'] == ['error_redis_error']
assert 'redis' in results.report(result)
legacy = deepcopy(result)
legacy['error_reasons'].pop('error_redis_error')
assert results.measure(legacy, {'metric': 'error_redis_error'}) is None
print('PASS: Redis Python/Lua/JSON, empty/escaped args, SLO selectors, report and comparison')
