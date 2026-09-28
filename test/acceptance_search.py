#!/usr/bin/env python3
"""Bounded SLO load search using the existing managed runner (prepared NIC required).

Input is a JSON scenario with duration_sec/target_cps and critical SLOs.
Every rate is repeated; all repetitions must pass. Invalid runs abort the search,
not count as an overloaded server. A passing ceiling is only a lower bound.
Legacy search.json harness. For structured capacity results and regression gates,
use snowtg.py capacity. Single-run maximum_sustainable_cps remains unmeasured.
"""
import argparse
from copy import deepcopy
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'traffic-gen'))
from snowtg_results import run, validate_assertions, write_json
from snowtg_capacity import search


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--minimum', type=int, default=100)
    parser.add_argument('--maximum', type=int, default=10000)
    parser.add_argument('--precision', type=int, default=100)
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--binary', type=Path, default=ROOT / 'traffic-gen/build/traffic-gen')
    parser.add_argument('scenario', type=Path)
    parser.add_argument('args', nargs=argparse.REMAINDER)
    opts = parser.parse_args()
    if not 0 < opts.minimum <= opts.maximum or opts.precision <= 0 or opts.repeats <= 0:
        parser.error('invalid search bounds, precision or repeat count')
    plan = json.loads(opts.scenario.read_text())
    if 'phases' in plan or 'duration_sec' not in plan or 'target_cps' not in plan:
        parser.error('use a fixed-duration, fixed-rate scenario')
    validate_assertions(plan)
    if not any(a.get('critical', True) for a in plan.get('assertions', [])):
        parser.error('at least one critical SLO is required')
    opts.output.mkdir(parents=True, exist_ok=False)
    result = dict(status='running', scenario=plan, minimum=opts.minimum,
                  maximum=opts.maximum, precision=opts.precision, repeats=opts.repeats,
                  rounds=[], limitations=[
                      'Finite-duration repeatability, not proof of indefinite sustainability.',
                      'Search assumes monotone SLO behavior; noise can move the bracket.',
                      'A passing maximum establishes a lower bound, not an exact capacity.'])

    def save():
        write_json(opts.output / 'search.json', result)

    def probe(cps):
        passed = True
        for repeat in range(opts.repeats):
            trial = deepcopy(plan)
            trial['target_cps'] = cps
            directory = opts.output / f'cps-{cps}-r{repeat + 1}'
            code = run(trial, opts.binary, opts.args, output=directory)
            measured = json.loads((directory / 'result.json').read_text())
            result['rounds'].append(dict(cps=cps, repeat=repeat + 1,
                result=str(directory.name + '/result.json'), exit_code=code,
                status=measured['status'], summary=measured['summary']))
            save()
            if code == 1:
                raise ValueError('invalid measurement at ' + str(directory))
            passed = passed and code == 0
        return passed

    save()
    try:
        result.update(search(probe, opts.minimum, opts.maximum, opts.precision))
    except (OSError, ValueError, KeyboardInterrupt) as error:
        result.update(status='invalid', reason=str(error))
        save()
        print(str(error), file=sys.stderr)
        return 1
    save()
    print(json.dumps({k: v for k, v in result.items() if k not in ('scenario', 'rounds')}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
