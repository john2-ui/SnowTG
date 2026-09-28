#!/usr/bin/env python3
"""Deterministic repeated searches, interval gates and the real CLI failure path."""
from copy import deepcopy
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'traffic-gen'))
import snowtg
import snowtg_capacity as capacity
import snowtg_results as results

plan = snowtg.scenario('capacity-test', duration=1, cps=1, concurrency=256,
    classes=[snowtg.dns('dns', '198.18.0.2', 'test.invalid')],
    assertions=[snowtg.assertion('success_rate', '>=', .99)])


def runner(limit=237, invalid_at=None, drift=False, intermittent=False):
    calls = []

    def run(trial, binary, args, output=None, timeout=None):
        cps = trial['target_cps']
        calls.append(cps)
        code = 1 if len(calls) == invalid_at else 0 if cps <= limit else 2
        if intermittent and calls.count(cps) == 2:
            code = 2
        output.mkdir()
        data = dict(schema_version=results.SCHEMA, scenario=trial,
            workload_sha256=results.digest(trial), valid=code != 1, exit_code=code,
            status={0: 'passed', 1: 'invalid', 2: 'failed'}[code],
            environment={'cpu_model': 'test', 'worker_lcores': [1 if not drift or len(calls) == 1 else 2]},
            runtime_arguments=args, build={'binary_sha256': 'fake'},
            summary={'maximum_sustainable_cps': None})
        results.write_json(output / 'result.json', data)
        return code

    return run, calls


with tempfile.TemporaryDirectory() as tmp, redirect_stdout(io.StringIO()):
    root = Path(tmp)

    def run_search(name, fake, **kwargs):
        with patch.object(results, 'run', fake):
            code = capacity.run_capacity(plan, Path('/bin/false'), ['--'], root / name,
                minimum=100, maximum=1000, precision=10, repeats=2, **kwargs)
        data = results.load_result(root / name / 'result.json')
        assert data['exit_code'] == code
        assert (root / name / 'report.html').is_file()
        return code, data

    fake, calls = runner()
    code, baseline = run_search('baseline', fake)
    assert code == 0 and baseline['capacity'] == dict(status='bracketed', passed_cps=237, failed_cps=243)
    assert baseline['summary']['maximum_sustainable_cps'] == 237
    assert all(calls.count(cps) == 2 for cps in calls)
    assert all(row['summary']['maximum_sustainable_cps'] is None for row in baseline['rounds'])
    assert all((root / 'baseline' / row['result']).is_file() for row in baseline['rounds'])
    code, same = run_search('same', runner()[0], baseline=root / 'baseline/result.json')
    assert code == 0 and same['comparison']['recommendation'] == 'accept_within_tested_scope'
    code, worse = run_search('worse', runner(150)[0], baseline=root / 'baseline/result.json')
    assert code == 2 and worse['valid'] and worse['comparison']['recommendation'] == 'reject'
    code, ceiling = run_search('ceiling', runner(1000)[0])
    assert code == 0 and ceiling['status'] == 'lower_bound_only'
    assert ceiling['summary'] == dict(maximum_sustainable_cps=None, sustainable_cps_lower_bound=1000)
    assert results.compare(ceiling, ceiling)['exit_code'] == 1
    assert results.compare(baseline, ceiling)['exit_code'] == 0
    assert results.compare(ceiling, worse)['exit_code'] == 2
    code, below = run_search('below', runner(0)[0])
    assert code == 2 and below['status'] == 'below_minimum'
    assert below['summary']['maximum_sustainable_cps'] is None
    fake, calls = runner(intermittent=True)
    assert run_search('intermittent', fake)[0] == 2 and calls == [100, 100]
    for index in (1, 2, 5):
        fake, calls = runner(invalid_at=index)
        code, invalid = run_search('invalid-' + str(index), fake)
        assert code == 1 and invalid['status'] == 'invalid' and len(calls) == index
        assert invalid['summary']['maximum_sustainable_cps'] is None
        assert results.compare(baseline, invalid)['exit_code'] == 1
    assert run_search('drift', runner(drift=True)[0])[0] == 1
    def interrupt(*args, **kwargs):
        raise KeyboardInterrupt()
    assert run_search('interrupted', interrupt)[1]['invalid_reasons'] == ['interrupted']
    def missing(*args, **kwargs):
        return 0
    assert run_search('missing-artifact', missing)[0] == 1

    # Bounds, not rounded point estimates, decide exact percentage boundaries.
    def bounds(low, high):
        data = deepcopy(baseline)
        data['capacity'].update(passed_cps=low, failed_cps=high)
        data['summary']['maximum_sustainable_cps'] = low
        data['summary']['sustainable_cps_lower_bound'] = low
        return data
    old = bounds(990, 1000)
    assert results.compare(old, bounds(900, 910), 10)['exit_code'] == 0
    assert results.compare(old, bounds(880, 890), 10)['exit_code'] == 2
    assert results.compare(old, bounds(890, 900), 10)['exit_code'] == 1
    for key in ('environment', 'runtime_arguments', 'search', 'scenario'):
        other = deepcopy(baseline)
        if key == 'environment': other[key]['cpu_model'] = 'different'
        if key == 'runtime_arguments': other[key] = ['-l', '9']
        if key == 'search': other[key]['repeats'] = 3
        if key == 'scenario':
            other[key]['assertions'][0]['value'] = .9
            other['workload_sha256'] = capacity.workload_hash(other[key])
        assert results.compare(baseline, other)['exit_code'] == 1
    other = deepcopy(baseline)
    other['scenario']['target_cps'] = 42
    other['scenario']['service'] = {'version': 'new'}
    assert results.compare(baseline, other)['exit_code'] == 0
    # Malformed bounds and fabricated maximum estimates cannot become baselines.
    for field, value in (('capacity', []), ('capacity', dict(status='bracketed', passed_cps=250, failed_cps=200)),
                         ('summary', dict(maximum_sustainable_cps=999, sustainable_cps_lower_bound=237))):
        bad = dict(baseline, **{field: value})
        results.write_json(root / 'bad.json', bad)
        try:
            results.load_result(root / 'bad.json')
        except ValueError:
            pass
        else:
            raise AssertionError('accepted malformed capacity')
    for value in (-1, 100, float('nan'), float('inf')):
        try:
            results.compare(baseline, baseline, value)
        except ValueError:
            pass
        else:
            raise AssertionError(value)

    source = root / 'scenario.py'
    source.write_text('plan = ' + repr(plan))
    for suffix in ('py', 'json'):
        source = root / ('scenario.' + suffix)
        source.write_text('plan = ' + repr(plan) if suffix == 'py' else json.dumps(plan))
        fake, calls = runner()
        with patch.object(results, 'run', fake):
            assert snowtg.main(['capacity', '--minimum', '100', '--maximum', '1000',
                '--precision', '10', '--output', str(root / suffix), str(source), '--', '-l', '1', '--']) == 0
        assert calls
    if os.environ.get('SNOWTG_LUA') or any(shutil.which(name) for name in ('lua', 'lua5.4', 'lua5.3')):
        lua = root / 'scenario.lua'
        lua.write_text('''local tg = require('snowtg')
return tg.scenario('capacity-test', {duration=1, cps=1, concurrency=256,
    classes={tg.dns('dns', '198.18.0.2', 'test.invalid')},
    assertions={tg.assertion('success_rate', '>=', .99)}})
''')
        fake, calls = runner()
        with patch.object(results, 'run', fake):
            assert snowtg.main(['capacity', '--minimum', '100', '--maximum', '1000', '--precision', '10',
                '--output', str(root / 'lua'), str(lua)]) == 0
        assert results.load_result(root / 'lua/result.json')['scenario'] == plan
    for options in (['--repeats', '0'], ['--precision', '0'], ['--maximum', '0'],
                    ['--max-capacity-drop-percent', 'nan'], ['--timeout', '0'], ['--emit-json', str(root / 'export.json')]):
        assert snowtg.main(['capacity', *options, str(source)]) == 1
    for name, bad in (('phased', dict(plan, phases=[])), ('no-slo', dict(plan, assertions=[]))):
        source.write_text(json.dumps(bad))
        assert snowtg.main(['capacity', '--output', str(root / name), str(source)]) == 1
        assert not (root / name).exists()
    source.write_text(json.dumps(plan))
    done = subprocess.run([sys.executable, str(ROOT / 'traffic-gen/snowtg.py'), 'capacity',
        '--output', str(root / 'native-failed'), '--binary', '/bin/false', str(source)],
        capture_output=True, text=True, timeout=15)
    assert done.returncode == 1, done.stderr
    failed = results.load_result(root / 'native-failed/result.json')
    assert failed['status'] == 'invalid' and len(failed['rounds']) == 1
    assert failed['summary']['maximum_sustainable_cps'] is None
    for path, code in (('same', 0), ('worse', 2), ('invalid-1', 1)):
        assert snowtg.main(['compare', str(root / 'baseline/result.json'), str(root / path / 'result.json'),
            '--max-capacity-drop-percent', '5', '--output', str(root / (path + '-comparison.json'))]) == code
    assert snowtg.main(['report', str(root / 'worse/result.json'), '--baseline', str(root / 'baseline/result.json'),
        '--output', str(root / 'offline.html')]) == 0
    assert 'reject' in (root / 'offline.html').read_text()
    malicious = dict(baseline, purpose='<script>alert(1)</script>')
    assert '<script>' not in results.report(malicious)
    assert results.compare(baseline, {'kind': 'run', 'summary': {}})['exit_code'] == 1

print('PASS: repeated capacity searches, invalid aborts, interval gates, CLI and offline reports')
