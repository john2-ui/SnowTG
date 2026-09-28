#!/usr/bin/env python3
"""Opt-in RESP2 test peer, not a Redis replacement.

GET snowtg:fault:{error,malformed,truncated,reset,timeout,recover}:<run-id>
selects deterministic failures; recover resets its first four requests only.
GET snowtg:peer:counts returns per-key request counts to detect replay.
"""
import argparse
import collections
import json
import socket
import socketserver
import struct
import threading
import time


def line(stream):
    value = stream.readline(1026)
    if not value.endswith(b'\r\n') or len(value) > 1026:
        raise ValueError('invalid/bounded RESP line')
    return value[:-2]


def read_command(stream):
    header = line(stream)
    if not header.startswith(b'*') or header[1:] not in (b'1', b'2', b'3'):
        raise ValueError('expected 1..3 arguments')
    args = []
    for _ in range(int(header[1:])):
        size = line(stream)
        if not size.startswith(b'$') or not size[1:].isdigit() or int(size[1:]) > 1024:
            raise ValueError('invalid argument size')
        n = int(size[1:])
        value = stream.read(n)
        if len(value) != n or stream.read(2) != b'\r\n':
            raise ValueError('truncated argument')
        args.append(value)
    return args


def bulk(value):
    return b'$-1\r\n' if value is None else b'$%d\r\n' % len(value) + value + b'\r\n'


class Peer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address):
        super().__init__(address, Handler)
        self.lock = threading.Lock()
        self.values = {}
        self.counts = collections.Counter()


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(15)
        try:
            while True:
                args = read_command(self.rfile)
                command = args[0].upper()
                key = args[1] if len(args) > 1 else b''
                with self.server.lock:
                    if command == b'GET' and key == b'snowtg:peer:counts':
                        self.wfile.write(bulk(json.dumps(dict(self.server.counts)).encode()))
                        continue
                    label = key.decode('utf-8')
                    self.server.counts[label] += 1
                    count = self.server.counts[label]
                mode = key.split(b':')[2] if key.startswith(b'snowtg:fault:') else b''
                if mode == b'reset' or (mode == b'recover' and count <= 4):
                    self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                    return
                if mode == b'timeout':
                    time.sleep(6)
                if mode == b'error':
                    self.wfile.write(b'-WRONGTYPE injected error\r\n')
                elif mode == b'malformed':
                    self.wfile.write(b'$x\r\n')
                elif mode == b'truncated':
                    self.wfile.write(b'$3\r\nx')
                    return
                elif mode in (b'recover', b'timeout'):
                    self.wfile.write(bulk(b'recovered'))
                elif command == b'PING' and len(args) == 1:
                    self.wfile.write(b'+PONG\r\n')
                elif command == b'SET' and len(args) == 3:
                    with self.server.lock:
                        self.server.values[key] = args[2]
                    self.wfile.write(b'+OK\r\n')
                elif command == b'GET' and len(args) == 2:
                    with self.server.lock:
                        value = self.server.values.get(key)
                    self.wfile.write(bulk(value))
                elif command == b'DEL' and len(args) == 2:
                    with self.server.lock:
                        existed = self.server.values.pop(key, None) is not None
                    self.wfile.write(b':1\r\n' if existed else b':0\r\n')
                else:
                    self.wfile.write(b'-ERR unsupported command\r\n')
        except (OSError, ValueError):
            return


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=6380)
    args = parser.parse_args()
    with Peer((args.bind, args.port)) as peer:
        print('RESP2 fault peer listening on', peer.server_address, flush=True)
        peer.serve_forever()
