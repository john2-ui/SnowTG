#!/usr/bin/env python3
"""Summarize retained profile-summary.json; output JSON medians and ranges."""
import json
import statistics
import sys

rows = json.load(open(sys.argv[1]))
assert len(rows) == 54
metrics = ('timer_sample_cycles', 'timer_cycles_per_success',
           'worker_cycles_per_success', 'steady_rps', 'steady_p99_ms',
           'timer_samples', 'worker_samples')
summary = []
for workers in (1, 4):
    for protocol in ('short', 'keepalive', 'dns'):
        group = dict(workers=workers, protocol=protocol, variants={})
        for variant in ('baseline', 'rte', 'wheel'):
            selected = [r for r in rows if (r['workers'], r['protocol'], r['variant']) ==
                        (workers, protocol, variant)]
            assert len(selected) == 3
            stats = {m: dict(median=statistics.median(r[m] for r in selected),
                             minimum=min(r[m] for r in selected),
                             maximum=max(r[m] for r in selected)) for m in metrics}
            stats['errors'] = {key: sum(r['error_reasons'].get(key, 0) for r in selected)
                               for key in selected[0]['error_reasons']}
            stats['residual_sockets'] = sum(r['summary']['drained_live_sockets'] for r in selected)
            stats['forced_cleanup'] = sum(r['summary']['tcp_forced_cleanup'] for r in selected)
            group['variants'][variant] = stats
        a, b = (group['variants'][v] for v in ('rte', 'wheel'))
        group['wheel_vs_rte_pct'] = {m: 100 * (b[m]['median'] / a[m]['median'] - 1)
                                    for m in metrics if a[m]['median']}
        group['timer_ranges_disjoint'] = (b['timer_sample_cycles']['maximum'] <
                                         a['timer_sample_cycles']['minimum'])
        summary.append(group)
print(json.dumps(summary, indent=2))
