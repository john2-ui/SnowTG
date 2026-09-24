"""Run via: python3 traffic-gen/snowtg.py <this-file> -- <EAL> -- <app-options>."""
import os
from snowtg import scenario, phase, http, dns

peer = os.environ.get("SNOWTG_PEER", "192.168.10.234")
plan = scenario("gateway-phased-http-dns", concurrency=256,
    phases=[
        phase("warmup",    3, 200),
        phase("ramp_up",   3, 1000, start=200),
        phase("steady",    4, 1000),
        phase("spike",     2, 2000),
        phase("cool_down", 3, 200),
    ],
    classes=[
        http("http_keepalive", peer, 8888, weight=6, keepalive=True),
        http("http_short", peer, 8888, weight=2),
        dns("dns", peer, "snowtg.test", 1053, weight=2),
    ])
