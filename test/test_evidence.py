#!/usr/bin/env python3
"""Deterministic evidence/clock/correlation regressions without a NIC or SSH."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'traffic-gen'))
import snowtg_evidence as e
import snowtg_results as r
from snowtg_monitor import sample

NS = 1_000_000_000
epoch = 1000 * NS
offset = 200_000_000
base = dict(schema_version=2, status='passed', valid=True, exit_code=0, environment={},
    scenario=dict(name='evidence', duration_sec=4, target_cps=10, classes=[]),
    summary=dict(duration_sec=4), groups=[], assertions=[], invalid_reasons=[],
    resources=dict(trend_complete=True, status='passed', workers=[]),
    clock_alignment=dict(status='bounded', epoch_wall_time_ns=epoch, uncertainty_ns=1000),
    timeline=[dict(worker=0, interval_start_sec=0, interval_sec=2, time_sec=2,
                   concurrency_blocked_turns_delta=12, skipped_delta=3, failed_delta=2,
                   complete_mean_us=250000, memory_paused=0, deferred_resource_delta=0),
              dict(worker=0, interval_start_sec=2, interval_sec=2, time_sec=4,
                   concurrency_blocked_turns_delta=0, skipped_delta=0, failed_delta=0,
                   complete_mean_us=1000, memory_paused=0, deferred_resource_delta=0)])


def host(t, drops=0):
    return dict(wall_time_ns=epoch + int(t * NS) + offset, monotonic_ns=int((t + 1) * NS), boot_id='boot',
                stat=f'cpu {10 + int(t * 10)} 0 0 {100 + int(t * 10)} 0 0 0 0 0 0\n',
                meminfo='MemTotal: 1000 kB\nMemAvailable: 400 kB\n',
                **{'net/dev': f'eth0: 100 2 0 {drops} 0 0 0 0 100 2 0 0 0 0 0 0\n',
                   'net/snmp': 'Tcp: RetransSegs InErrs\nTcp: 0 0\n'})


def kinds(data):
    return {f['category'] for w in data['windows'] for f in w['findings']}


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    def jsonl(name, rows):
        (root / name).write_text(''.join(json.dumps(row) + '\n' for row in rows))
    jsonl('host.jsonl', [host(0), host(2, 3), host(4, 3)])
    jsonl('requests.jsonl', [dict(time_ns=epoch + NS + offset, protocol='http', mode='slow'),
        dict(time_ns=epoch + NS + offset, protocol='http', duration_ns=250_000_000)])
    jsonl('faults.jsonl', [dict(start_ns=epoch + NS // 2 + offset, end_ns=epoch + NS + offset,
                               category='network', label='<script>alert(1)</script>')])
    spec = dict(schema_version=1, reference_host='client', client_host='client', window_sec=2,
        clock_offsets=[dict(host='server', offset_ns=offset, uncertainty_ns=1_000_000)],
        sources=[dict(kind='host', host='server', role='server', path='host.jsonl', interfaces=['eth0']),
                 dict(kind='requests', host='server', path='requests.jsonl'),
                 dict(kind='faults', host='server', path='faults.jsonl')])
    manifest = root / 'manifest.json'
    def attach(name, source=base, config=None):
        manifest.write_text(json.dumps(spec if config is None else config))
        output = root / name; output.mkdir()
        result = deepcopy(source)
        data = e.attach(result, e.load_manifest(manifest), output)
        assert result['summary'] == source['summary'] and result['valid'] == source['valid']
        assert result['exit_code'] == source['exit_code']
        return result, data, output

    result, data, output = attach('combined')
    assert data['status'] == 'available', data['issues']
    assert {'client_concurrency_guard', 'network_signals', 'server_slow', 'unattributed'} <= kinds(data)
    assert not data['windows'][1]['findings']
    assert data['host_intervals'][0]['deltas']['net.eth0.rx_drops'] == 3
    assert data['host_intervals'][0]['memory_kib']['MemAvailable'] == 400
    assert data['host_intervals'][0]['cpu_busy_percent'] == 50
    req = next(iter(data['windows'][0]['requests'].values()))
    assert req['arrivals'] == req['completions'] == req['slow'] == 1
    for source in data['sources']:
        assert hashlib.sha256((output / source['snapshot']).read_bytes()).hexdigest() == source['sha256']
    html = r.report(result)
    assert '<script>alert(1)</script>' not in html and '&lt;script&gt;' in html

    local = deepcopy(base)
    local['environment']['dataplane'] = dict(timer_hz='1000', epoch_cycles='1100', dataplane_start_cycles='1000')
    local['nic_samples'] = [dict(elapsed_cycles=2100, nic_interval_cycles=2000, nic_delta_valid=1,
        nic_imissed=1, nic_ierrors=2, nic_oerrors=0, nic_rx_nombuf=0)]
    local['timeline'][0]['resource_failures_delta'] = {'res_tcp_tcb_exhausted': 1}
    _, data, _ = attach('local-resources', source=local)
    assert data['nic_intervals'][0]['start_sec'] == 0
    assert {'client_resource_pressure', 'client_nic_pressure'} <= kinds(data)
    local['resources']['trend_complete'] = False
    _, data, _ = attach('incomplete-resources', source=local)
    assert 'client_resource_pressure' not in kinds(data)
    legacy_owner = deepcopy(base)
    legacy_owner['resources']['workers'] = [dict(worker=0, metrics={'flow': dict(timeline=[
        dict(time_sec=2, current=3, exhausted=1, unavailable=0, busy=0, limit=0),
        dict(time_sec=4, current=0, exhausted=1, unavailable=0, busy=0, limit=0)])})]
    _, data, _ = attach('legacy-owner-trends', source=legacy_owner)
    assert 'client_resource_pressure' in kinds(data)
    assert not data['windows'][1]['findings']
    local['nic_samples'][0]['nic_delta_valid'] = 0
    _, data, _ = attach('invalid-nic', source=local)
    assert 'client_nic_pressure' not in kinds(data)

    # Missing/uncertain/stepped clocks cannot support cross-host conclusions.
    missing = deepcopy(spec); missing['clock_offsets'] = []
    _, data, _ = attach('missing-clock', config=missing)
    assert 'network_signals' not in kinds(data) and 'server_slow' not in kinds(data)
    assert data['status'] == 'partial'

    # Malformed field types are recorded, not allowed to crash reporting.
    jsonl('host.jsonl', [dict(host(0), stat=[]), dict(host(1), processes={'1': None})])
    _, data, _ = attach('malformed-host')
    assert data['sources'][0]['malformed'] == 2
    jsonl('host.jsonl', [host(0), host(2, 3), host(4, 3)])
    wide = deepcopy(spec); wide['clock_offsets'][0]['uncertainty_ns'] = 3 * NS
    _, data, _ = attach('wide-clock', config=wide)
    assert 'network_signals' not in kinds(data) and 'server_slow' not in kinds(data)
    old = deepcopy(base); old.pop('clock_alignment')
    _, data, _ = attach('old-clock', source=old)
    assert data['run_start_reference_ns'] is None and 'network_signals' not in kinds(data)
    declared = deepcopy(spec); declared['run_start'] = dict(host='client', time_ns=epoch, uncertainty_ns=2_000_000)
    _, data, _ = attach('declared-clock', source=old, config=declared)
    assert 'network_signals' in kinds(data)
    unstable = deepcopy(base); unstable['clock_alignment'] = dict(status='unstable')
    _, data, _ = attach('unstable-native', source=unstable, config=declared)
    assert 'network_signals' not in kinds(data)
    jump = host(4, 4); jump['wall_time_ns'] += NS
    jsonl('host.jsonl', [host(0), host(2, 3), jump])
    _, data, _ = attach('jump-host')
    assert 'network_signals' not in kinds(data) and 'server_slow' not in kinds(data)
    assert any('跳变' in s for s in data['issues'])
    client_jump = deepcopy(spec); client_jump['sources'][0]['host'] = 'client'
    _, data, _ = attach('jump-client-invalidates-server', config=client_jump)
    assert 'network_signals' not in kinds(data) and 'server_slow' not in kinds(data)

    # Counter resets are not negative drops; interface selection is respected.
    jsonl('host.jsonl', [host(0, 3), host(2, 1)])
    config = deepcopy(spec); config['sources'] = config['sources'][:1]
    _, data, _ = attach('reset-counter', config=config)
    assert 'net.eth0.rx_drops' not in data['host_intervals'][0]['deltas']
    assert 'network_signals' not in kinds(data)
    assert any('回退' in s for s in data['issues'])
    jsonl('host.jsonl', [host(0), host(2, 3)])
    config['sources'][0]['interfaces'] = ['eth1']
    _, data, _ = attach('different-interface', config=config)
    assert 'network_signals' not in kinds(data)

    # Old arrival-only request logs cannot establish service time.
    jsonl('requests.jsonl', [dict(time_ns=epoch + NS + offset, protocol='http', mode='slow')])
    with (root / 'requests.jsonl').open('a') as f:
        f.write('readiness log\n{"time_ns":')
    _, data, _ = attach('legacy-requests')
    assert 'server_slow' not in kinds(data)
    assert data['sources'][1]['malformed'] == 2
    assert any('处理耗时' in s for s in data['issues'])
    missing = deepcopy(spec); missing['sources'][0]['path'] = 'not-there'
    _, data, _ = attach('missing-file', config=missing)
    assert data['status'] == 'partial'

    # Existing controller logs are state changes, never shell commands to run.
    jsonl('faults.jsonl', [dict(kind='command', command='do not run'),
        dict(kind='netem', time_ns=epoch + offset, settings='loss 100%'),
        dict(kind='netem', time_ns=epoch + NS + offset, settings='delay 0ms'),
        dict(kind='fault_state', time_ns=epoch + offset, config={'http': 'slow'}),
        dict(kind='fault_state', time_ns=epoch + NS + offset, config={})])
    _, data, _ = attach('controller')
    assert len(data['fault_windows']) == 2
    assert 'server_fault_window' in kinds(data)

    # Official offline command produces a derivative result; original stays intact.
    original = root / 'result.json'; original.write_text(json.dumps(base))
    original_bytes = original.read_bytes()
    assert r.cli(['correlate', str(original), '--evidence', str(manifest), '--output', str(root / 'offline')]) == 0
    assert original.read_bytes() == original_bytes
    assert (root / 'offline/report.html').exists()
    loaded = r.load_result(root / 'offline/result.json')
    assert loaded['correlation_origin']['sha256'] == hashlib.sha256(original_bytes).hexdigest()
    assert r.cli(['report', str(root / 'offline/result.json'), '--output', str(root / 'offline-again.html')]) == 0

    # Unsupported manifests are rejected before touching a run directory.
    for update in (dict(schema_version=2), dict(window_sec=0), dict(server_slow_ms=float('nan')),
                   dict(clock_offsets=[dict(host='s', offset_ns=True, uncertainty_ns=0)])):
        manifest.write_text(json.dumps(dict(spec, **update)))
        try:
            e.load_manifest(manifest)
        except ValueError:
            pass
        else:
            raise AssertionError(update)
    subprocess.run([sys.executable, str(ROOT / 'traffic-gen/snowtg.py'), 'monitor', '--samples', '2',
                    '--interval', '.01', '--output', str(root / 'monitor.jsonl')], check=True)
    assert len((root / 'monitor.jsonl').read_text().splitlines()) == 2

log = ('clock_anchor cycles=1000 wall_time_ns=1000000000000 monotonic_ns=1000000000 uncertainty_ns=1000\n'
       'clock_anchor cycles=3000 wall_time_ns=1002000000000 monotonic_ns=3000000000 uncertainty_ns=1000\n')
clock = e.native_clock(log, dict(timer_hz='1000', epoch_cycles='1100'))
assert clock['status'] == 'bounded' and clock['epoch_wall_time_ns'] == 1000100000000
assert e.native_clock(log.replace('1002000000000', '1003000000000'), dict(timer_hz='1000', epoch_cycles='1100'))['status'] == 'unstable'
assert e.native_clock('', {})['status'] == 'unavailable'
assert 'wall_time_ns' in sample()
print('PASS: clock bounds, mixed evidence, concurrency guard, unknown attribution, corrupted/legacy evidence, snapshots and CLI')
