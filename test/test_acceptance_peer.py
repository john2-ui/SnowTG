#!/usr/bin/env python3
"""Exercise the real HTTP/DNS fault fixture on loopback; no DPDK needed."""
import http.client
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    control = root / 'fault.json'
    control.write_text('{}')
    hp, dp = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_DGRAM)
    with (root / 'peer.log').open('w') as log:
        pressure = '--pressure' in sys.argv
        peer = subprocess.Popen([sys.executable, str(Path(__file__).with_name('pressure_peer.py' if pressure else 'workflow_peer.py')),
            '--ip', '127.0.0.1', *([] if pressure else ['--bind', '127.0.0.1']), '--http-port', str(hp),
            '--dns-port', str(dp), '--fault-file', str(control)], stdout=log, stderr=log)
        try:
            for _ in range(100):
                if peer.poll() is not None:
                    raise AssertionError((root / 'peer.log').read_text())
                try:
                    with socket.create_connection(('127.0.0.1', hp), timeout=.1):
                        break
                except OSError:
                    time.sleep(.02)
            else:
                raise AssertionError('peer did not start')

            def mode(protocol, name):
                temp = root / 'next.json'
                temp.write_text(json.dumps({protocol: name, 'delay_ms': 100}))
                temp.replace(control)
                if pressure:
                    time.sleep(.04)  # fixture polls its control file every 20 ms

            for name in ('normal', 'unavailable', 'close', 'slow', 'reset', 'truncate', 'malformed'):
                mode('http', name)
                client = http.client.HTTPConnection('127.0.0.1', hp, timeout=2)
                start = time.monotonic()
                try:
                    client.request('GET', '/')
                    response = client.getresponse()
                    response.read()
                    assert name not in ('reset', 'truncate', 'malformed'), name
                    assert response.status == (503 if name == 'unavailable' else 200)
                    if name == 'slow':
                        assert time.monotonic() - start >= .09
                except (http.client.HTTPException, ConnectionError):
                    assert name in ('reset', 'truncate', 'malformed'), name
                finally:
                    client.close()
            query = struct.pack('!HHHHHH', 1, 0x100, 1, 0, 0, 0) + b'\x06snowtg\x04test\0\0\1\0\1'
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.settimeout(.3)
                for name in ('normal', 'error', 'malformed', 'drop', 'slow'):
                    mode('dns', name)
                    sock.sendto(query, ('127.0.0.1', dp))
                    try:
                        packet, _ = sock.recvfrom(4096)
                    except socket.timeout:
                        assert name == 'drop'
                        continue
                    assert name != 'drop'
                    if name == 'malformed':
                        assert len(packet) < 12
                    else:
                        assert packet[:2] == query[:2]
                        assert packet[3] & 15 == (2 if name == 'error' else 0)
        finally:
            peer.terminate()
            try:
                peer.wait(timeout=3)
            except subprocess.TimeoutExpired:
                peer.kill()
                peer.wait()
print('PASS: HTTP reset/truncate/malformed/503/close/delay and DNS timeout/error/malformed/delay')
