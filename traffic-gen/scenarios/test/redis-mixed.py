"""Fixed-key Redis traffic mixed with HTTP/DNS; no ordering between classes."""
from snowtg import scenario, redis, http, dns

plan = scenario("redis-mixed", duration=10, cps=100, concurrency=32, classes=[
    redis("ping", "198.18.0.2", command="PING"),
    redis("get", "198.18.0.2", command="GET", key="snowtg:key"),
    redis("set", "198.18.0.2", command="SET", key="snowtg:key", value="hello"),
    http("http", "198.18.0.2", 8080, keepalive=True),
    dns("dns", "198.18.0.2", "snowtg.test", 1053),
])
