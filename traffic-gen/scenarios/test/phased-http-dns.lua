-- python3 traffic-gen/snowtg.py <this-file> -- <EAL> -- <app-options>
local tg = require("snowtg")
local peer = os.getenv("SNOWTG_PEER") or "192.168.10.234"

return tg.scenario("gateway-phased-http-dns", {concurrency=256,
    phases={
        tg.phase("warmup",    3, 200),
        tg.phase("ramp_up",   3, 1000, {start=200}),
        tg.phase("steady",    4, 1000),
        tg.phase("spike",     2, 2000),
        tg.phase("cool_down", 3, 200),
    },
    classes={
        tg.http("http_keepalive", peer, 8888, {weight=6, keepalive=true}),
        tg.http("http_short", peer, 8888, {weight=2}),
        tg.dns("dns", peer, "snowtg.test", 1053, {weight=2}),
    }})
