#!/usr/bin/env python3
"""Check socket-demo or stack-demo from a peer; no network configuration changes."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import socket


def tcp_echo(host, port):
    def exchange(index):
        payload = bytes([index]) * (128 * 1024 + 17)
        with socket.create_connection((host, port), timeout=10) as peer:
            peer.sendall(payload)
            # FIN must not discard the echo still queued at the server.
            peer.shutdown(socket.SHUT_WR)
            reply = bytearray()
            while True:
                block = peer.recv(997)
                if not block:
                    break
                reply.extend(block)
            assert reply == payload, (index, len(reply), len(payload))

    with socket.create_connection((host, port), timeout=10) as idle:
        # Prove this connection was accepted before opening the other clients.
        probe = b"idle connection probe"
        idle.sendall(probe)
        reply = bytearray()
        while len(reply) < len(probe):
            block = idle.recv(len(probe) - len(reply))
            assert block, "idle connection closed before probe echo"
            reply.extend(block)
        assert reply == probe
        # A sequential server will now stall in recv on this live connection.
        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(exchange, range(8)))
    print("PASS: idle connection plus 8 TCP clients, 128 KiB+17 each, "
          "short reads and half-close")


def udp_echo(host, port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.settimeout(5)
        peer.connect((host, port))
        for payload in (b"", b"hello", bytes(range(256)) * 5):
            peer.send(payload)
            assert peer.recv(65535) == payload
    print("PASS: UDP empty, 5-byte and 1280-byte datagrams")


def client_peer(host, port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((host, port))
        listener.listen(1)
        listener.settimeout(30)
        print("READY: run socket-demo tcp-client", flush=True)
        peer, _ = listener.accept()
        with peer:
            peer.settimeout(10)
            while True:
                block = peer.recv(7)
                if not block:
                    break
                peer.sendall(block)
    print("PASS: asynchronous TCP client completed echo")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("tcp-echo", "udp-echo", "client-peer"))
    parser.add_argument("host", help="demo IP, or local bind IP for client-peer")
    parser.add_argument("port", type=int)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be in 1..65535")
    {"tcp-echo": tcp_echo, "udp-echo": udp_echo,
     "client-peer": client_peer}[args.mode](args.host, args.port)
