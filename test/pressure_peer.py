#!/usr/bin/env python3
"""Async, synthetic HTTP/DNS fault peer for bounded pressure acceptance.

No throughput claim is made for this fixture. Control is an atomically replaced
local JSON file: http/dns mode, delay_ms, body_bytes (HTTP, up to 1 MiB).
Request events are logged before injection so retries/excess requests are visible.
"""
import argparse
import asyncio
import json
from pathlib import Path
import socket
import struct
import time


def event(protocol, mode, duration_ns=None):
    row = dict(time_ns=time.time_ns(), protocol=protocol, mode=mode,
               event='arrival' if duration_ns is None else 'complete')
    if duration_ns is not None:
        row['duration_ns'] = duration_ns
    print(json.dumps(row), flush=True)


async def serve(args):
    config = {}

    async def refresh():
        nonlocal config
        while True:
            value = json.loads(args.fault_file.read_text())
            for key, allowed in [('http', {'normal', 'reset', 'truncate', 'malformed', 'unavailable', 'slow', 'close'}),
                                 ('dns', {'normal', 'drop', 'error', 'malformed', 'slow'})]:
                if value.get(key, 'normal') not in allowed:
                    raise ValueError('invalid ' + key + ' mode')
            if not 0 <= value.get('delay_ms', 200) <= 10000 or not 0 <= value.get('body_bytes', 1024) <= 1048576:
                raise ValueError('invalid delay/body size')
            config = value
            await asyncio.sleep(.02)

    async def http(reader, writer):
        try:
            while True:
                headers = await reader.readuntil(b'\r\n\r\n')
                if not headers.startswith(b'GET '):
                    return
                current = config
                mode = current.get('http', 'normal')
                event('http', mode)
                started = time.monotonic_ns()
                try:
                    if mode == 'reset':
                        writer.get_extra_info('socket').setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                        writer.transport.abort()
                        return
                    if mode == 'slow':
                        await asyncio.sleep(current.get('delay_ms', 200) / 1000)
                    if mode == 'malformed':
                        writer.write(b'NOT-HTTP\r\n\r\n')
                        await writer.drain()
                        return
                    body = b'x' * current.get('body_bytes', 1024)
                    close = mode in ('truncate', 'close') or b'connection: close' in headers.lower()
                    status = b'503 Unavailable' if mode == 'unavailable' else b'200 OK'
                    length = len(body) + (20 if mode == 'truncate' else 0)
                    writer.write(b'HTTP/1.1 ' + status + b'\r\nContent-Length: ' + str(length).encode() +
                                 b'\r\nConnection: ' + (b'close' if close else b'keep-alive') + b'\r\n\r\n' + body)
                    await writer.drain()
                    if close:
                        return
                finally:
                    event('http', mode, time.monotonic_ns() - started)
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
            pass
        finally:
            writer.close()

    class DNS(asyncio.DatagramProtocol):
        def connection_made(self, transport):
            self.transport = transport

        def datagram_received(self, packet, address):
            if len(packet) < 17:
                return
            end = packet.find(b'\0', 12) + 5
            if end < 17 or end > len(packet):
                return
            current = config
            mode = current.get('dns', 'normal')
            started = time.monotonic_ns()
            event('dns', mode)
            if mode == 'drop':
                return
            if mode == 'malformed':
                response = packet[:2] + b'\x80'
            else:
                error = mode == 'error'
                response = packet[:2] + struct.pack('!HHHHH', 0x8182 if error else 0x8180, 1, 0 if error else 1, 0, 0) + packet[12:end]
                if not error:
                    response += b'\xc0\x0c' + struct.pack('!HHIH', 1, 1, 0, 4) + socket.inet_aton(args.ip)
            def send_response():
                self.transport.sendto(response, address)
                event('dns', mode, time.monotonic_ns() - started)

            if mode == 'slow':
                asyncio.get_running_loop().call_later(current.get('delay_ms', 200) / 1000, send_response)
            else:
                send_response()

    server = await asyncio.start_server(http, args.ip, args.http_port, backlog=4096)
    transport, _ = await asyncio.get_running_loop().create_datagram_endpoint(DNS, local_addr=(args.ip, args.dns_port))
    print('pressure peer ready', flush=True)
    try:
        async with server:
            await refresh()
    finally:
        transport.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ip', required=True)
    parser.add_argument('--http-port', type=int, default=18081)
    parser.add_argument('--dns-port', type=int, default=15354)
    parser.add_argument('--fault-file', type=Path, required=True)
    asyncio.run(serve(parser.parse_args()))
