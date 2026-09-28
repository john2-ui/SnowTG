#!/usr/bin/env python3
"""Local self-check of the opt-in peer/probe, independent of DPDK or Redis."""
import json
import socket
import threading

from redis_peer import Peer
from test_redis_live import command

with Peer(('127.0.0.1', 0)) as peer:
    thread = threading.Thread(target=peer.serve_forever, daemon=True)
    thread.start()
    host, port = peer.server_address
    try:
        assert command(host, port, 'PING') == b'PONG'
        assert command(host, port, 'SET', '雪', '') == b'OK'
        assert command(host, port, 'GET', '雪') == b''
        assert command(host, port, 'GET', 'missing') is None
        for mode in ('error', 'malformed', 'truncated', 'reset'):
            try:
                command(host, port, 'GET', 'snowtg:fault:' + mode + ':unit')
            except (OSError, AssertionError, ValueError):
                pass
            else:
                raise AssertionError(mode)
        key = 'snowtg:fault:recover:unit'
        for _ in range(4):
            try:
                command(host, port, 'GET', key)
            except (OSError, ValueError):
                pass
            else:
                raise AssertionError('missing reset')
        assert command(host, port, 'GET', key) == b'recovered'
        counts = json.loads(command(host, port, 'GET', 'snowtg:peer:counts'))
        assert counts[key] == 5
        with socket.create_connection((host, port), timeout=.1) as sock:
            key = b'snowtg:fault:timeout:unit'
            sock.sendall(b'*2\r\n$3\r\nGET\r\n$%d\r\n' % len(key) + key + b'\r\n')
            try:
                sock.recv(1)
            except socket.timeout:
                pass
            else:
                raise AssertionError('missing delay')
    finally:
        peer.shutdown()
        thread.join()
print('PASS: local RESP2 peer probes, fault modes, recovery and request counts')
