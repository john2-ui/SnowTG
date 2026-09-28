#!/usr/bin/env python3
"""Small deterministic check of search limits, precision and invalid probes."""
from acceptance_search import search

seen = []
def probe(rate):
    seen.append(rate)
    return rate <= 237

r = search(probe, 100, 1000, 10)
assert r['status'] == 'bracketed'
assert r['passed_cps'] <= 237 < r['failed_cps']
assert r['failed_cps'] - r['passed_cps'] <= 10
assert len(seen) == len(set(seen))
assert search(lambda _: True, 100, 1000, 10) == dict(status='lower_bound_only', passed_cps=1000, failed_cps=None)
assert search(lambda _: False, 100, 1000, 10) == dict(status='below_minimum', passed_cps=None, failed_cps=100)
assert search(lambda _: True, 100, 100, 10)['status'] == 'lower_bound_only'
def invalid(_):
    raise ValueError('invalid run')
try:
    search(invalid, 100, 1000, 10)
except ValueError:
    pass
else:
    raise AssertionError('invalid run became a capacity result')
print('PASS: load search brackets, bounds and invalid-run propagation')
