#!/usr/bin/env python3
"""SLO semantics, weighted quantiles, comparison gates, HTML escaping and failures."""
from copy import deepcopy
import json
import math
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'traffic-gen'))
from snowtg import assertion, scenario, phase, http
import snowtg_results as r

plan = scenario('test', phases=[phase('steady', 10, 10)],
    classes=[http('a', '198.18.0.2'), http('b', '198.18.0.2')],
    assertions=[assertion('success_rate', '>=', .99),
                assertion('latency_ms', '<', 1, quantile=.99)],
    purpose='<script>alert(1)</script>', service={'version': 'test'})
r.validate_assertions(plan)
for changes in [dict(metric='unknown'), dict(op='~'), dict(value=float('nan')),
                dict(phase='missing'), {'class': 'missing'}, dict(critical='false'),
                dict(quantile=.99), dict(extra=1)]:
    bad = deepcopy(plan)
    bad['assertions'] = [dict(plan['assertions'][0], **changes)]
    try:
        r.validate_assertions(bad)
        raise AssertionError(bad)
    except ValueError:
        pass
for q in (0, 1.1, float('inf'), True):
    bad = deepcopy(plan)
    bad['assertions'] = [assertion('latency_ms', '<', 1, quantile=q)]
    try:
        r.validate_assertions(bad)
        raise AssertionError(q)
    except ValueError:
        pass

groups = []
for name, count, bins, maximum in [('a', 98, [[103, 98]], 100), ('b', 2, [[303, 1], [927, 1]], 900)]:
    hist = dict(samples=count, max_us=maximum, buckets=bins)
    groups.append(dict(phase='steady', **{'class':name}, protocol='http', planned=count,
        attempted=count, skipped=0, admitted=count, success=count, failed=0, start_failed=0,
        latency={metric:r.distribution(hist) for metric in r.LATENCIES}))
result = dict(schema_version=1, scenario=plan, workload_sha256='same', purpose=plan['purpose'],
    valid=True, status='passed', groups=groups, environment={'cpu_model':'unit', 'dataplane':{}},
    summary={'success_rps':10, 'p95_ms':.103, 'p99_ms':.303, 'error_rate':0,
             'tx_alloc_fail':0, 'drained_live_sockets':0, 'maximum_sustainable_cps':None},
    errors={'fail_io':0}, resource_peaks={'tx_peak':{'value':3}}, invalid_reasons=[])
assert r.measure(result, {'metric':'latency_ms', 'quantile':.99}) == .303
assert r.measure(result, {'metric':'latency_ms', 'quantile':1}) == .9
assert r.measure(result, {'metric':'success_rps', 'class':'b'}) == .2
assert r.measure(result, {'metric':'latency_ms', 'quantile':.99, 'class':'missing'}) is None
result['assertions'] = r.evaluate(result)
assert all(a['passed'] for a in result['assertions'])
assert r.compare(result, result)['recommendation'] == 'accept_within_tested_scope'
loss = deepcopy(result)
loss['groups'][0].update(planned=198, skipped=100)
assert r.measure(loss, {'metric':'success_rate'}) == .5
assert r.measure(loss, {'metric':'error_rate'}) == .5
assert r.measure(loss, {'metric':'admitted_success_rate'}) == 1
assert not r.evaluate(loss)[0]['passed']
for key, value in [('success_rps', 9), ('p95_ms', 1), ('p99_ms', 1), ('error_rate', .01)]:
    candidate = deepcopy(result)
    candidate['summary'][key] = value
    diff = r.compare(result, candidate)
    assert key in diff['regressions'] and diff['recommendation'] == 'reject', diff
candidate = deepcopy(result)
candidate['errors']['fail_io'] = 1
assert r.compare(result, candidate)['new_error_types'] == ['fail_io']
candidate['workload_sha256'] = 'different'
assert not r.compare(result, candidate)['comparable']
candidate = deepcopy(result)
candidate['runtime_arguments'] = ['-l','3,4','--']
assert not r.compare(result, candidate)['comparable']
candidate = deepcopy(result)
candidate['scenario']['assertions'][0]['value'] = .1
assert not r.compare(result, candidate)['comparable']
candidate = deepcopy(result)
candidate['summary']['p99_ms'] = None
assert r.compare(result, candidate)['recommendation'] == 'inconclusive'
assert r.change(0, 10)['percent'] is None
assert r.change(0, 0)['percent'] == 0
assert r.change(None, None)['reason'] == 'not measured'
assert '<script>alert(1)</script>' not in r.report(result)
assert '&lt;script&gt;alert(1)&lt;/script&gt;' in r.report(result)

with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    r.write_json(temp/'result.json', result)
    code = r.cli(['report', str(temp/'result.json'), '--output', str(temp/'report.html')])
    assert code == 0 and (temp/'report.html').exists()
    assert r.cli(['compare', str(temp/'result.json'), str(temp/'result.json'), '--output', str(temp/'diff.json')]) == 0
    try:
        r.cli(['compare', str(temp/'result.json'), str(temp/'result.json'), '--output', str(temp/'result.json')])
        raise AssertionError('clobbered input')
    except ValueError:
        pass
    output = temp/'startup-failed'
    assert r.run(plan, Path('/bin/false'), [], output) == 1
    data = r.load_result(output/'result.json')
    assert not data['valid'] and data['status'] == 'invalid' and data['invalid_reasons']
    assert all(a['actual'] is None and not a['passed'] for a in data['assertions'])
    assert (output/'report.html').exists()
    try:
        r.run(plan, Path('/bin/false'), [], output)
        raise AssertionError('overwrote run')
    except FileExistsError:
        pass
    sleeper = temp/'sleeping binary'
    sleeper.write_text('#!'+sys.executable+'\nimport time\ntime.sleep(60)\n')
    sleeper.chmod(0o700)
    assert r.run(plan, sleeper, [], temp/'timeout', timeout=.1) == 1
    assert 'run timeout' in r.load_result(temp/'timeout/result.json')['invalid_reasons']
    for suffix in ('py', 'lua'):
        if suffix == 'lua' and not (os.environ.get('SNOWTG_LUA') or any(
                shutil.which(name) for name in ('lua', 'lua5.4', 'lua5.3'))):
            continue
        exported = temp/(suffix+'.json')
        subprocess.run([sys.executable, str(ROOT/'traffic-gen/snowtg.py'), '--emit-json', str(exported),
                        str(ROOT/('traffic-gen/scenarios/test/acceptance-http-dns.'+suffix))], check=True)
        r.validate_assertions(json.loads(exported.read_text()))
    if (temp/'lua.json').exists():
        assert json.loads((temp/'py.json').read_text()) == json.loads((temp/'lua.json').read_text())
print('PASS: SLO semantics, histogram merging, comparison guards, failure artifacts and HTML escaping')
