"""Bounded, offline evidence correlation. Coincidence is never proof of causality.

All clocks use Unix nanoseconds; offset = host wall clock - reference wall clock.
No SSH, remote commands, clock adjustment, or per-request tracing is performed.
"""
import hashlib
import json
import math
from pathlib import Path
import re

NS = 1_000_000_000
MAX_FILE = 512 * 1024 * 1024
MAX_LINE = 2 * 1024 * 1024
MAX_INTERVALS = 20000
LIMITATIONS = [
    '同期证据只支持候选解释，不证明因果；多种原因可同时存在。',
    '并发保护是发压端已观测行为，其上游原因仍可能是网络、服务端或客户端自身。',
    '主机 CPU、内核 TCP 和接口计数包含其他业务；DPDK 绕过内核，内核零增量不证明链路正常。',
    '没有共享请求 ID，不做逐请求匹配，不用客户端延迟减服务端耗时推算网络延迟。',
    '服务端 duration_ns 是日志定义的处理区间，可包含等待和写回背压，不等同于纯计算时间。',
    '采样区间、时钟误差和未采集的请求均保留；没有记录不等于没有故障。',
]


def integer(value, name, minimum=0):
    if type(value) is not int or not minimum <= value <= 2**63 - 1:
        raise ValueError('invalid ' + name)
    return value


def load_manifest(path):
    path = Path(path).resolve()
    if path.stat().st_size > MAX_LINE:
        raise ValueError('evidence manifest too large')
    data = json.loads(path.read_text())
    allowed = {'schema_version', 'reference_host', 'client_host', 'clock_offsets',
               'sources', 'run_start', 'window_sec', 'server_slow_ms'}
    if not isinstance(data, dict) or set(data) - allowed or type(data.get('schema_version')) is not int or data['schema_version'] != 1:
        raise ValueError('invalid evidence manifest schema')
    for key in ('reference_host', 'client_host'):
        if not isinstance(data.get(key), str) or not data[key] or len(data[key]) > 128:
            raise ValueError('invalid ' + key)
    for key, default in (('window_sec', 5), ('server_slow_ms', 100)):
        value = data.setdefault(key, default)
        if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
            raise ValueError('invalid ' + key)
    sources = data.get('sources', [])
    if not isinstance(sources, list) or len(sources) > 32:
        raise ValueError('evidence supports at most 32 sources')
    for source in sources:
        if (not isinstance(source, dict) or set(source) - {'kind', 'host', 'path', 'role', 'interfaces'} or
                source.get('kind') not in ('host', 'requests', 'faults') or
                not isinstance(source.get('host'), str) or not source['host'] or
                not isinstance(source.get('path'), str) or not source['path']):
            raise ValueError('invalid evidence source')
        if source.get('role', 'server') not in ('client', 'server', 'network'):
            raise ValueError('invalid evidence role')
        if 'interfaces' in source and (not isinstance(source['interfaces'], list) or not source['interfaces'] or
                                      any(not isinstance(x, str) or not x for x in source['interfaces'])):
            raise ValueError('invalid interfaces')
    offsets = data.get('clock_offsets', [])
    if isinstance(offsets, str):
        offset_path = path.parent / offsets
        if offset_path.stat().st_size > MAX_LINE:
            raise ValueError('clock offsets too large')
        offsets = json.loads(offset_path.read_text())
    if not isinstance(offsets, list) or len(offsets) > 1024:
        raise ValueError('invalid clock_offsets')
    for row in offsets:
        if not isinstance(row, dict) or not isinstance(row.get('host'), str):
            raise ValueError('invalid clock offset')
        integer(row.get('offset_ns'), 'offset_ns', -(2**63))
        integer(row.get('uncertainty_ns'), 'uncertainty_ns')
    data['clock_offsets'] = offsets
    if 'run_start' in data:
        row = data['run_start']
        if not isinstance(row, dict) or not isinstance(row.get('host'), str):
            raise ValueError('invalid run_start')
        integer(row.get('time_ns'), 'run_start.time_ns')
        integer(row.get('uncertainty_ns'), 'run_start.uncertainty_ns')
    data['_base'] = str(path.parent)
    return data


def native_clock(log, metadata):
    rows = [dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', line))
            for line in log.splitlines() if line.startswith('clock_anchor ')]
    required = {'cycles', 'wall_time_ns', 'monotonic_ns', 'uncertainty_ns'}
    if len(rows) < 2 or any(not required <= row.keys() for row in rows):
        return dict(status='unavailable', reason='原生运行缺少首尾时钟锚点')
    hz, epoch = int(metadata.get('timer_hz', 0)), int(metadata.get('epoch_cycles', 0))
    if hz <= 0 or epoch <= 0:
        return dict(status='unavailable', reason='缺少原生 epoch/frequency')
    first, last = rows[0], rows[-1]
    wall_delta = last['wall_time_ns'] - first['wall_time_ns']
    mono_delta = last['monotonic_ns'] - first['monotonic_ns']
    error = first['uncertainty_ns'] + last['uncertainty_ns']
    if mono_delta <= 0 or last['cycles'] <= first['cycles'] or abs(wall_delta - mono_delta) > max(1_000_000, error):
        return dict(status='unstable', reason='首尾锚点显示时钟跳变/倒退，禁止跨机归因', anchors=rows)
    drift = abs((last['cycles'] - first['cycles']) * NS // hz - wall_delta)
    return dict(status='bounded', epoch_wall_time_ns=first['wall_time_ns'] + (epoch - first['cycles']) * NS // hz,
                uncertainty_ns=error + drift + 1000, anchors=rows,
                reason='首尾锚点只约束采样精度；未证明两台主机已同步')


def host_counters(row, interfaces):
    """Read existing acceptance_monitor JSONL without depending on Linux at import."""
    if any(not isinstance(row.get(k, ''), str) for k in ('stat', 'meminfo', 'net/dev', 'net/snmp', 'net/netstat')) or not isinstance(row.get('processes', {}), dict):
        raise ValueError('invalid host counters')
    values = {}
    for line in row.get('stat', '').splitlines():
        fields = line.split()
        if fields and fields[0] == 'cpu':
            nums = list(map(int, fields[1:9]))  # guest times already included in user/nice
            if len(nums) == 8:
                values['cpu_total'] = sum(nums)
                values['cpu_busy'] = sum(nums) - nums[3] - nums[4]
    for line in row.get('net/dev', '').splitlines():
        if ':' not in line:
            continue
        iface, fields = line.split(':', 1)
        iface, nums = iface.strip(), list(map(int, fields.split()))
        if len(nums) < 16 or (interfaces and iface not in interfaces):
            continue
        for name, index in (('rx_bytes', 0), ('rx_errors', 2), ('rx_drops', 3),
                            ('tx_bytes', 8), ('tx_errors', 10), ('tx_drops', 11)):
            values['net.' + iface + '.' + name] = nums[index]
    for field in ('net/snmp', 'net/netstat'):
        lines = row.get(field, '').splitlines()
        for i in range(0, len(lines) - 1, 2):
            keys, nums = lines[i].split(), lines[i + 1].split()
            if not keys or not nums or keys[0] != nums[0] or len(keys) != len(nums):
                continue
            for key, value in zip(keys[1:], nums[1:]):
                if key in ('RetransSegs', 'InErrs', 'InDiscards', 'OutDiscards', 'ListenOverflows', 'ListenDrops', 'TCPTimeouts'):
                    values[keys[0][:-1] + '.' + key] = int(value)
    for pid, proc in row.get('processes', {}).items():
        # /proc/PID/stat comm may contain spaces or parentheses; fields after
        # its final ')' start at field 3. Starttime disambiguates PID reuse.
        if not isinstance(proc, dict) or not isinstance(proc.get('stat'), str):
            raise ValueError('invalid process stat')
        fields = proc['stat'].rsplit(')', 1)[1].split()
        values['process.' + pid + '.' + fields[19] + '.cpu_ticks'] = int(fields[11]) + int(fields[12])
    if any(v < 0 for v in values.values()):
        raise ValueError('negative cumulative host counter')
    return values


def _snapshot(source, destination):
    """Freeze the initial file length: growing collectors cannot make copying endless."""
    digest = hashlib.sha256()
    with source.open('rb') as src, destination.open('xb') as dst:
        size = source.stat().st_size
        if size > MAX_FILE:
            raise ValueError('source exceeds 512 MiB')
        remaining = size
        while remaining:
            chunk = src.read(min(1024 * 1024, remaining))
            if not chunk:
                raise ValueError('source truncated during snapshot')
            dst.write(chunk); digest.update(chunk); remaining -= len(chunk)
    return digest.hexdigest(), size


def attach(result, manifest, output):
    """Attach advisory correlation; do not modify SLO, validity or exit code."""
    manifest = manifest or dict(client_host='client', reference_host='client', sources=[], clock_offsets=[], window_sec=5, server_slow_ms=100)
    evidence = dict(schema_version=1, status='local_only', sources=[], issues=[], windows=[], host_intervals=[],
                    fault_windows=[], nic_intervals=[], client_intervals=[dict(r) for r in result.get('timeline', [])], limitations=LIMITATIONS[:],
                    reference_host=manifest['reference_host'], server_slow_ms=manifest['server_slow_ms'])
    result['correlation'] = evidence
    issues = evidence['issues']
    unstable_hosts = set()
    clocks = {manifest['reference_host']: (0, 0)}
    # Envelope all supplied measurements, not the most optimistic sample.
    for row in manifest['clock_offsets']:
        host = row['host']
        lo, hi = row['offset_ns'] - row['uncertainty_ns'], row['offset_ns'] + row['uncertainty_ns']
        if host == manifest['reference_host']:
            continue
        if host in clocks:
            off, err = clocks[host]
            lo, hi = min(lo, off - err), max(hi, off + err)
        clocks[host] = ((lo + hi) // 2, (hi - lo + 1) // 2)
    evidence['clocks'] = {h: dict(offset_ns=o, uncertainty_ns=u) for h, (o, u) in clocks.items()}
    evidence['limitations'].append('外部时钟偏差由用户声明适用于本轮；静态偏差不能证明采样间没有漂移或跳变。')
    anchor = manifest.get('run_start')
    clock = result.get('clock_alignment', {})
    if anchor is None and clock.get('status') == 'bounded':
        anchor = dict(host=manifest['client_host'], time_ns=clock['epoch_wall_time_ns'], uncertainty_ns=clock['uncertainty_ns'])
    if clock.get('status') == 'unstable':
        anchor = None  # User-supplied epoch cannot repair a clock jump during this run.
    origin, origin_error = None, 0
    if anchor and anchor['host'] in clocks:
        off, err = clocks[anchor['host']]
        origin, origin_error = anchor['time_ns'] - off, anchor['uncertainty_ns'] + err
    else:
        issues.append('缺少可信的负载起点/客户端时钟映射；外部证据不参与同期归因')
    evidence['run_start_reference_ns'] = origin
    evidence['run_start_uncertainty_ns'] = origin_error if origin is not None else None
    duration = max(float(result.get('summary', {}).get('duration_sec', 0)),
                   max((p.get('time_sec', 0) for p in result.get('timeline', [])), default=0))
    width = max(float(manifest['window_sec']), math.ceil(duration / 10000))
    evidence['window_sec'] = width
    for i in range(max(1, math.ceil(duration / width))):
        evidence['windows'].append(dict(start_sec=i * width, end_sec=(i + 1) * width,
            client_intervals=[], nic_intervals=[], host_intervals=[], faults=[], requests={}, findings=[]))
    windows = evidence['windows']

    def indexes(start, end):
        if end < 0 or start >= len(windows) * width:
            return range(0)
        return range(max(0, math.floor(start / width)), min(len(windows), max(math.floor(start / width) + 1, math.ceil(end / width))))

    for i, row in enumerate(evidence['client_intervals']):
        for index in indexes(row.get('interval_start_sec', row['time_sec'] - row['interval_sec']), row['time_sec']):
            windows[index]['client_intervals'].append(i)

    # NIC CSV timestamps have a different origin than the workers. Old runs
    # without that origin remain unaligned; NIC deltas are already intervals.
    meta = result.get('environment', {}).get('dataplane', {})
    if 'dataplane_start_cycles' in meta and int(meta.get('timer_hz', 0)) > 0:
        hz = int(meta['timer_hz'])
        if any(not row['nic_delta_valid'] for row in result.get('nic_samples', [])):
            issues.append('部分 NIC 区间计数无效，不参与归因')
        for row in result.get('nic_samples', []):
            end = (int(meta['dataplane_start_cycles']) + row['elapsed_cycles'] - int(meta['epoch_cycles'])) / hz
            start = end - row['nic_interval_cycles'] / hz
            if row['nic_delta_valid'] and indexes(start, end):
                ref = len(evidence['nic_intervals'])
                evidence['nic_intervals'].append(dict(start_sec=start, end_sec=end,
                    deltas={k: row[k] for k in ('nic_imissed', 'nic_ierrors', 'nic_oerrors', 'nic_rx_nombuf')}))
                for index in indexes(start, end):
                    windows[index]['nic_intervals'].append(ref)
    elif result.get('nic_samples'):
        issues.append('旧版 NIC 采样缺少时间原点，不参与时间窗关联')
    resource_trusted = result.get('resources', {}).get('trend_complete', False)
    if not resource_trusted:
        issues.append('owner 资源趋势缺失或不完整，不用其计数推断资源压力')
    else:
        # Older saved results already contain owner trends but do not embed
        # their deltas in the client timeline. Reuse those samples as well.
        client_rows = {(r['worker'], r['time_sec']): r for r in evidence['client_intervals']}
        for worker in result.get('resources', {}).get('workers', []):
            for name, metric in worker['metrics'].items():
                old = {}
                for row in metric['timeline']:
                    client = client_rows.get((worker['worker'], row['time_sec']))
                    if client is not None:
                        client.setdefault('resource_current', {})[name] = row['current']
                        for field in ('exhausted', 'unavailable', 'busy', 'limit'):
                            client.setdefault('resource_failures_delta', {})['res_' + name + '_' + field] = row[field] - old.get(field, 0)
                    old = row

    def position(timestamp, host):
        integer(timestamp, 'timestamp')
        if origin is None or host not in clocks:
            return None, None
        off, err = clocks[host]
        return (timestamp - off - origin) / NS, (err + origin_error) / NS

    def add_fault(row, src):
        start, error = position(row['start_ns'], src['host'])
        end, _ = position(row['end_ns'], src['host'])
        if row['category'] not in ('client', 'network', 'server') or row['end_ns'] < row['start_ns']:
            raise ValueError('invalid fault interval')
        if start is None:
            return
        item = dict(row, source=src['id'], start_sec=start, end_sec=end, uncertainty_sec=error,
                    aligned=error <= width / 2)
        if not indexes(start - error, end + error):
            src['outside'] += 1
            return
        if len(evidence['fault_windows']) >= MAX_INTERVALS:
            raise ValueError('too many fault intervals')
        ref = len(evidence['fault_windows']); evidence['fault_windows'].append(item)
        if item['aligned']:
            for index in indexes(start - error, end + error):
                windows[index]['faults'].append(ref)

    root = Path(output) / 'evidence'
    if manifest.get('sources'):
        root.mkdir(exist_ok=False)
        saved = {k: v for k, v in manifest.items() if k != '_base'}
        saved['sources'] = [dict(s, path='source-' + str(i) + '.jsonl') for i, s in enumerate(manifest['sources'])]
        (root / 'manifest.json').write_text(json.dumps(saved, ensure_ascii=False, indent=2) + '\n')
    for number, spec in enumerate(manifest.get('sources', [])):
        src = dict(spec, id='source-' + str(number), records=0, matched=0, completions=0, malformed=0, ignored=0, outside=0, issues=[])
        evidence['sources'].append(src)
        source = Path(manifest['_base']) / spec['path']
        snapshot = root / (src['id'] + '.jsonl')
        previous = None
        states = {}
        if origin is None or src['host'] not in clocks:
            src['issues'].append('缺少时钟映射，未对齐')
        elif (clocks[src['host']][1] + origin_error) / NS > width / 2:
            src['issues'].append('时钟误差超过半个关联窗口，禁止同期归因')
        try:
            src['sha256'], src['bytes'] = _snapshot(source, snapshot)
            src['snapshot'] = 'evidence/' + snapshot.name
            with snapshot.open('rb') as stream:
                while True:
                    raw = stream.readline(MAX_LINE + 1)
                    if not raw:
                        break
                    if len(raw) > MAX_LINE:
                        raise ValueError('source line exceeds 2 MiB')
                    if not raw.endswith(b'\n'):
                        src['malformed'] += 1
                        continue
                    try:
                        row = json.loads(raw)
                        if not isinstance(row, dict):
                            raise ValueError('record must be an object')
                        src['records'] += 1
                        if src['kind'] == 'host':
                            wall = integer(row['wall_time_ns'], 'wall_time_ns')
                            mono = integer(row['monotonic_ns'], 'monotonic_ns')
                            counters = host_counters(row, src.get('interfaces'))
                            start, error = position(wall, src['host'])
                            current = (wall, mono, counters, start, error, row.get('boot_id'))
                            if previous:
                                pw, pm, pc, ps, pe, boot = previous
                                if mono <= pm or wall <= pw or (boot and row.get('boot_id') != boot) or abs((wall - pw) - (mono - pm)) > 1_000_000:
                                    if len(src['issues']) < 100:
                                        src['issues'].append('主机时钟跳变/重启/乱序；该主机静态时钟映射失效')
                                    unstable_hosts.add(src['host'])
                                elif start is not None:
                                    if not indexes(ps - error, start + error):
                                        src['outside'] += 1
                                        previous = current
                                        continue
                                    delta = {k: counters[k] - pc[k] for k in counters.keys() & pc.keys()}
                                    reset = [k for k, v in delta.items() if v < 0]
                                    for k in reset:
                                        del delta[k]
                                    if reset and len(src['issues']) < 100:
                                        src['issues'].append('计数器回退：' + ', '.join(reset))
                                    total = delta.get('cpu_total', 0)
                                    memory = dict(re.findall(r'^(MemTotal|MemAvailable):\s+(\d+)', row.get('meminfo', ''), re.M))
                                    ticks = row.get('ticks_per_second')
                                    process_cpu = {k: v * NS / ((mono - pm) * ticks) for k, v in delta.items()
                                                   if k.endswith('.cpu_ticks')} if type(ticks) is int and ticks > 0 else {}
                                    item = dict(source=src['id'], role=src.get('role', 'server'), host=src['host'],
                                                start_sec=ps, end_sec=start, uncertainty_sec=error,
                                                aligned=error <= width / 2, deltas=delta,
                                                memory_kib={k: int(v) for k, v in memory.items()}, process_cpu_cores=process_cpu,
                                                cpu_busy_percent=100 * delta['cpu_busy'] / total if total > 0 and 0 <= delta.get('cpu_busy', -1) <= total else None)
                                    if len(evidence['host_intervals']) >= MAX_INTERVALS:
                                        raise OverflowError('too many host intervals')
                                    ref = len(evidence['host_intervals']); evidence['host_intervals'].append(item)
                                    src['matched'] += 1
                                    if item['aligned']:
                                        for index in indexes(ps - error, start + error):
                                            windows[index]['host_intervals'].append(ref)
                            previous = current
                        elif src['kind'] == 'requests':
                            t, error = position(row['time_ns'], src['host'])
                            protocol = row.get('protocol')
                            if protocol not in ('http', 'dns', 'redis'):
                                raise ValueError('invalid protocol')
                            duration_ns = row.get('duration_ns')
                            if duration_ns is not None:
                                integer(duration_ns, 'duration_ns')
                                src['completions'] += 1
                            if t is None:
                                continue
                            if not 0 <= t < len(windows) * width:
                                src['outside'] += 1
                                continue
                            bucket = windows[int(t // width)]['requests'].setdefault(src['id'] + ':' + protocol,
                                dict(source=src['id'], protocol=protocol, arrivals=0, completions=0, slow=0,
                                     duration_sum_ns=0, duration_max_ns=None, boundary_uncertain=0, aligned=error <= width / 2))
                            src['matched'] += 1
                            bucket['boundary_uncertain'] += int(int(max(0, t - error) // width) != int((t + error) // width))
                            if duration_ns is None:
                                bucket['arrivals'] += 1
                            else:
                                bucket['completions'] += 1
                                bucket['slow'] += int(duration_ns / 1e6 >= manifest['server_slow_ms'])
                                bucket['duration_sum_ns'] += duration_ns
                                bucket['duration_max_ns'] = max(bucket['duration_max_ns'] or 0, duration_ns)
                        else:
                            if 'start_ns' in row:
                                add_fault(row, src)
                                continue
                            kind = row.get('kind')
                            if kind not in ('fault_state', 'netem'):
                                src['ignored'] += 1
                                continue
                            now = integer(row['time_ns'], 'time_ns')
                            group = 'network' if kind == 'netem' else 'server'
                            if group in states:
                                old = states.pop(group)
                                add_fault(dict(start_ns=old[0], end_ns=now, category=group, label=old[1]), src)
                            config = row.get('settings') if kind == 'netem' else row.get('config', {})
                            if kind == 'fault_state' and not isinstance(config, dict):
                                raise ValueError('invalid fault config')
                            if kind == 'netem' and not isinstance(config, str):
                                raise ValueError('invalid netem settings')
                            # Legacy controller restores a no-op qdisc with this
                            # exact setting; arbitrary commands are never run.
                            active = ' '.join(config.split()) not in ('', 'delay 0ms') if kind == 'netem' else any(v not in ('normal', None) for k, v in config.items() if k in ('http', 'dns'))
                            if active:
                                states[group] = (now, json.dumps(config, ensure_ascii=False))
                    except (ValueError, KeyError, TypeError, UnicodeError, IndexError, RecursionError):
                        src['malformed'] += 1
            if states:
                src['issues'].append('故障日志缺少结束记录，未推定开放区间的结束时间')
            if src['malformed']:
                src['issues'].append('损坏/非 JSON/截断行：' + str(src['malformed']))
            if not src['records']:
                src['issues'].append('没有可用记录')
            if src['kind'] == 'requests' and not src['completions']:
                src['issues'].append('请求日志未记录处理耗时，不能判断服务端是否变慢')
            if src['kind'] in ('host', 'requests') and not src['matched']:
                src['issues'].append('没有可对齐且与本轮重叠的样本')
        except (OSError, ValueError, OverflowError) as exc:
            src['issues'].append(str(exc))
        src['issues'] = list(dict.fromkeys(src['issues']))
        issues.extend(src['id'] + ': ' + p for p in src['issues'])

    # A jump invalidates that host's static offset for all its evidence kinds.
    origin_unstable = manifest['client_host'] in unstable_hosts or manifest['reference_host'] in unstable_hosts
    untrusted = {s['id'] for s in evidence['sources'] if origin_unstable or s['host'] in unstable_hosts}
    for row in evidence['host_intervals'] + evidence['fault_windows']:
        if row['source'] in untrusted:
            row['aligned'] = False
    for window in windows:
        for row in window['requests'].values():
            if row['source'] in untrusted:
                row['aligned'] = False
        client = [evidence['client_intervals'][i] for i in window['client_intervals']]
        guard = any((r.get('concurrency_blocked_turns_delta') or 0) > 0 for r in client)
        degraded = any((r.get('failed_delta') or 0) > 0 or (r.get('completed_error_rate') or 0) > 0 or (r.get('skipped_delta') or 0) > 0 or
                       (r.get('complete_mean_us') or 0) >= manifest['server_slow_ms'] * 1000 for r in client)
        findings = window['findings']
        if guard:
            findings.append(dict(category='client_concurrency_guard', strength='observed', evidence=window['client_intervals']))
        if any(r.get('memory_paused') or (r.get('deferred_resource_delta') or 0) > 0 or
               (resource_trusted and any(v > 0 for v in r.get('resource_failures_delta', {}).values())) for r in client):
            findings.append(dict(category='client_resource_pressure', strength='observed', evidence=window['client_intervals']))
        nic_pressure = [i for i in window['nic_intervals'] if any(evidence['nic_intervals'][i]['deltas'][k] > 0
                        for k in ('nic_imissed', 'nic_rx_nombuf'))]
        nic_errors = [i for i in window['nic_intervals'] if any(evidence['nic_intervals'][i]['deltas'][k] > 0
                      for k in ('nic_ierrors', 'nic_oerrors'))]
        if nic_pressure:
            findings.append(dict(category='client_nic_pressure', strength='observed', nic_intervals=nic_pressure))
        network = [i for i in window['faults'] if evidence['fault_windows'][i]['aligned'] and evidence['fault_windows'][i]['category'] == 'network']
        server_faults = [i for i in window['faults'] if evidence['fault_windows'][i]['aligned'] and evidence['fault_windows'][i]['category'] == 'server']
        client_faults = [i for i in window['faults'] if evidence['fault_windows'][i]['aligned'] and evidence['fault_windows'][i]['category'] == 'client']
        host_network = [i for i in window['host_intervals'] if evidence['host_intervals'][i]['aligned'] and any(
            v > 0 and (k.endswith(('_drops', '_errors', '.RetransSegs', '.InErrs', '.InDiscards', '.OutDiscards', '.TCPTimeouts')))
            for k, v in evidence['host_intervals'][i]['deltas'].items())]
        service = [key for key, r in window['requests'].items() if r['aligned'] and r['slow'] > 0]
        if degraded and (network or host_network or nic_errors):
            findings.append(dict(category='network_signals', strength='coincident', faults=network, host_intervals=host_network, nic_intervals=nic_errors))
        if service:
            findings.append(dict(category='server_slow', strength='coincident' if degraded else 'observed', requests=service))
        if degraded and server_faults:
            findings.append(dict(category='server_fault_window', strength='coincident', faults=server_faults))
        if degraded and client_faults:
            findings.append(dict(category='client_fault_window', strength='coincident', faults=client_faults))
        if degraded:
            findings.append(dict(category='unattributed', strength='unattributed',
                reason='无法确定各因素对客户端异常的贡献；没有逐请求对应关系' if network or host_network or service else
                       '现有证据不足以区分网络、服务端处理及客户端调度'))
    evidence['status'] = ('partial' if issues else 'available') if manifest.get('sources') else 'local_only'
    return evidence


LABELS = {'client_concurrency_guard': '客户端并发保护已触发', 'client_resource_pressure': '客户端资源压力',
          'client_nic_pressure': '客户端 NIC 丢包/接收缓冲不足（设备计数）',
          'server_fault_window': '同期服务端故障记录（用户声明）',
          'client_fault_window': '同期客户端故障记录（用户声明）',
          'network_signals': '同期网络异常证据（候选）', 'server_slow': '服务端处理耗时超过阈值', 'unattributed': '仍不可归因'}


def render(result, table, pre):
    data = result.get('correlation')
    if not data:
        return '<h2>客户端、网络与服务端证据关联</h2><p>未记录跨主机证据，不能据此归因。</p>'

    def requests(window):
        summaries = []
        for row in window['requests'].values():
            count = row['completions']
            timing = (f"均值 {row['duration_sum_ns'] / count / 1e6:.2f} ms，最大 {row['duration_max_ns'] / 1e6:.2f} ms"
                      if count else '无处理耗时')
            summaries.append(f"{row['source']}/{row['protocol']}：到达 {row['arrivals']}，完成 {count}，"
                f"慢 {row['slow']}，{timing}，边界不确定 {row['boundary_uncertain']}" +
                ('（时钟未对齐）' if not row['aligned'] else ''))
        return '；'.join(summaries) or '无对齐记录'

    parts = ['<h2>客户端、网络与服务端证据关联</h2>', pre({k: data[k] for k in
        ('status', 'reference_host', 'run_start_reference_ns', 'run_start_uncertainty_ns', 'window_sec', 'server_slow_ms', 'clocks')}),
        table(['时间窗（秒）', '证据与结论', '客户端区间', 'NIC 区间', '主机区间', '故障区间', '请求日志'], [
            (f"{w['start_sec']:g}–{w['end_sec']:g}", '；'.join(LABELS[f['category']] for f in w['findings']) or '未发现足够证据（不代表正常）',
             str(w['client_intervals']), str(w.get('nic_intervals', [])), str(w['host_intervals']), str(w['faults']), requests(w))
            for w in data['windows']]),
        '<details><summary>证据来源、哈希与缺口</summary>', pre(data['sources']), pre(data['issues']), '</details>',
        '<details><summary>客户端与主机采样区间、故障时间窗</summary>', pre(data['client_intervals']),
        pre(data.get('nic_intervals', [])), pre(data['host_intervals']), pre(data['fault_windows']), '</details>', pre(data['limitations'])]
    return '\n'.join(parts)
