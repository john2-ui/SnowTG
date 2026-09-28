#!/usr/bin/env python3
"""Redis acceptance through the native dataplane; requires a prepared NIC/AF_PACKET.

Pass --peer, an unused output directory, then EAL/app args after --.
Optional --fault-port selects redis_peer.py on the same peer for failure/recovery.
No NIC binding, Redis configuration changes or service restarts are performed.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'traffic-gen'))
from snowtg import scenario, phase, redis, assertion
from redis_peer import line


def command(peer, port, *args):
    """Small control-plane probe; load always runs through the native flow layer."""
    encoded = [a.encode() for a in args]
    wire = b'*%d\r\n' % len(args) + b''.join(b'$%d\r\n' % len(a) + a + b'\r\n' for a in encoded)
    with socket.create_connection((peer, port), timeout=10) as sock, sock.makefile('rb') as stream:
        sock.sendall(wire)
        header = line(stream)
        if header.startswith(b'+'):
            return header[1:]
        if header == b'$-1':
            return None
        if header.startswith(b'$') and header[1:].isdigit():
            n = int(header[1:])
            assert n <= 65536, header
            value = stream.read(n)
            assert len(value) == n and stream.read(2) == b'\r\n'
            return value
        raise AssertionError(header)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--peer', required=True)
    parser.add_argument('--port', type=int, default=6379)
    parser.add_argument('--fault-port', type=int)
    parser.add_argument('--binary', default=os.environ.get('SNOWTG_TEST_BINARY', str(ROOT / 'traffic-gen/build/traffic-gen')))
    parser.add_argument('output', type=Path)
    parser.add_argument('args', nargs=argparse.REMAINDER)
    opts = parser.parse_args()
    native_args = opts.args[1:] if opts.args[:1] == ['--'] else opts.args
    opts.output.mkdir(parents=True, exist_ok=False)
    run_id = uuid.uuid4().hex
    key = 'snowtg:test:' + run_id
    assert command(opts.peer, opts.port, 'PING') == b'PONG'

    def run(name, cls, reason=None, recovery=False):
        count = 12 if recovery else 8
        checks = [assertion('drained_live_sockets', '==', 0)]
        if recovery:
            checks += [assertion('success_rate', '==', 1, protocol='redis', phase='recovery'),
                       assertion('error_reset', '==', 4)]
        else:
            checks += [assertion('error_rate' if reason else 'success_rate', '==', 1, protocol='redis')]
            if reason:
                checks += [assertion('error_' + reason, '==', count)]
        plan = scenario(name, concurrency=16, classes=[cls], assertions=checks,
            phases=[phase('fault', 1, 4), phase('recovery', 2, 4)] if recovery else [phase('steady', 2, 4)])
        source = opts.output / (name + '.json')
        source.write_text(json.dumps(plan))
        done = subprocess.run([sys.executable, str(ROOT / 'traffic-gen/snowtg.py'), 'run',
            '--binary', opts.binary, '--output', str(opts.output / name), str(source), '--', *native_args],
            text=True, capture_output=True, timeout=180)
        (opts.output / (name + '.log')).write_text(done.stdout + done.stderr)
        result = json.loads((opts.output / name / 'result.json').read_text())
        assert done.returncode == 0 and result['valid'], (name, result)
        assert result['summary']['planned'] == count
        assert result['summary']['start_failed'] == result['summary']['skipped'] == 0
        assert result['summary']['success'] == (8 if recovery else 0 if reason else count)
        assert result['resources']['status'] == 'passed'
        assert all(m['final']['current'] == 0 for w in result['resources']['workers'] for m in w['metrics'].values())
        if not reason and not recovery:
            reused = result['final_counters']['connections_reused']
            assert reused > 0 if cls['redis']['keepalive'] else reused == 0
            assert result['final_counters']['http_success_total'] == 0
            assert result['summary']['request_success_rps'] == 4
            assert result['groups'][0]['latency']['complete_success']['samples'] == count
        print('PASS:', name, flush=True)
        return result

    for name, config in [('ping', dict(command='PING')),
                         ('set', dict(command='SET', key=key, value='snowtg-redis')),
                         ('get', dict(command='GET', key=key)),
                         ('miss', dict(command='GET', key=key + ':missing')),
                         ('short', dict(command='PING', keepalive=False))]:
        run(name, redis(name, opts.peer, opts.port, **config))
        if name == 'set':
            assert command(opts.peer, opts.port, 'GET', key) == b'snowtg-redis'
    # Only delete the unique key created by this acceptance run.
    with socket.create_connection((opts.peer, opts.port), timeout=10) as sock:
        data = key.encode()
        sock.sendall(b'*2\r\n$3\r\nDEL\r\n$%d\r\n' % len(data) + data + b'\r\n')
        sock.recv(1024)
    if opts.fault_port is not None:
        for mode, reason in [('error', 'redis_error'), ('malformed', 'parse'),
                             ('truncated', 'peer_eof'), ('reset', 'reset'),
                             ('timeout', 'response_timeout'), ('recover', None)]:
            fault_key = 'snowtg:fault:' + mode + ':' + run_id
            run(mode, redis(mode, opts.peer, opts.fault_port, command='GET', key=fault_key),
                reason=reason, recovery=mode == 'recover')
            counts = json.loads(command(opts.peer, opts.fault_port, 'GET', 'snowtg:peer:counts'))
            assert counts[fault_key] == (12 if mode == 'recover' else 8), 'unexpected request replay'
    else:
        print('SKIP: fault/recovery acceptance needs --fault-port with redis_peer.py')


if __name__ == '__main__':
    main()
