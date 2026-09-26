#!/usr/bin/env python3
"""Deterministic integration peer, separate from production benchmark services.

HTTP :18080 and DNS :15353. Only synthetic tokens/data; HTTP logs go to stdout.
Run on the test peer with --ip set to its reachable IPv4 address.
"""
import argparse
import http.server
import json
import socketserver
import struct
import threading
import time
import urllib.parse

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ip', required=True)
parser.add_argument('--http-port', type=int, default=18080)
parser.add_argument('--dns-port', type=int, default=15353)
args = parser.parse_args()


# Correctness fixture, not a throughput server. Tokens contain quotes, a backslash,
# and Unicode; /use verifies extraction/escaping across requests. Repeated and empty
# headers exercise incremental capture independently of JSON parsing.
class HTTP(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_GET(self):
        self.respond()

    def do_POST(self):
        self.respond()

    def respond(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        path = urllib.parse.urlsplit(self.path).path
        status, payload = 200, {'ok': True, 'token': 'a"\\猫', 'route': True, 'items': [7, 'x/y']}
        if path.startswith('/status/'):
            status = int(path.rsplit('/', 1)[1])
        if path == '/use':
            try:
                posted = json.loads(body)
                if posted['token'] != 'a"\\猫' or self.headers.get('X-Route') not in ('A', 'B'):
                    status = 422
            except (ValueError, KeyError):
                status = 422
        # Delay this peer handler to trigger client deadlines; generator workers never sleep.
        if path == '/slow':
            time.sleep(0.2)
        data = json.dumps(payload, ensure_ascii=False).encode()
        if path == '/large': data = b'"' + b'x'*65536 + b'"'
        if path == '/invalid': data = b'{"x":1 trailing}'
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('X-Repeat', 'first')
        self.send_header('x-repeat', 'second')
        self.send_header('X-Empty', '')
        if path == '/chunked':
            self.send_header('Transfer-Encoding', 'chunked')
        else:
            self.send_header('Content-Length', str(len(data)))
        if path == '/close':
            self.send_header('Connection', 'close')
            self.close_connection = True
        self.end_headers()
        try:
            if path == '/chunked':
                # Small chunks can split UTF-8 and JSON tokens; actual TCP segmentation varies.
                # The C protocol test supplies the deterministic one-byte receive boundary case.
                for start in range(0, len(data), 3):
                    chunk = data[start:start+3]
                    self.wfile.write(('%x\r\n' % len(chunk)).encode()+chunk+b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
            else:
                self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


# Ordinary queries return A; alias returns CNAME plus matching A; missing returns
# a structurally valid unrelated A to reject arbitrary first-address selection.
class DNS(socketserver.BaseRequestHandler):
    def handle(self):
        packet, sock = self.request
        if len(packet) < 17: return
        cursor, labels = 12, []
        while cursor < len(packet) and packet[cursor]:
            size=packet[cursor]; labels.append(packet[cursor+1:cursor+1+size].decode('ascii')); cursor+=size+1
        name='.'.join(labels)
        question=packet[12:cursor+5]
        address=bytes(map(int,args.ip.split('.')))
        record=b'\xc0\x0c'+struct.pack('!HHIH',1,1,30,4)+address
        if name.startswith('missing.'):
            record=b'\x09unrelated\x04test\x00'+struct.pack('!HHIH',1,1,30,4)+address
        count=1
        if name.startswith('alias.'):
            target=b'\x06target\x04test\x00'
            record=b'\xc0\x0c'+struct.pack('!HHIH',5,1,30,len(target))+target+target+struct.pack('!HHIH',1,1,30,4)+address
            count=2
        sock.sendto(packet[:2]+struct.pack('!HHHHH',0x8180,1,count,0,0)+question+record,self.client_address)


server = http.server.ThreadingHTTPServer(('0.0.0.0', args.http_port), HTTP)
dns = socketserver.ThreadingUDPServer(('0.0.0.0', args.dns_port), DNS)
threading.Thread(target=dns.serve_forever, daemon=True).start()
print('workflow peer ready', flush=True)
server.serve_forever()
