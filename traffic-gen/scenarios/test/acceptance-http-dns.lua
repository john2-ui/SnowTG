-- snowtg.py run --output debug/acceptance <this-file> -- <EAL> -- <app>
local tg = require('snowtg')
local peer = os.getenv('SNOWTG_PEER') or '192.168.10.234'
return tg.scenario('acceptance-http-dns', {concurrency=256,
    purpose='HTTP/DNS phased load and SLO acceptance',
    service={name='nginx + dnsmasq', version=os.getenv('SNOWTG_SERVICE_VERSION') or 'unspecified'},
    phases={tg.phase('warmup', 2, 200), tg.phase('ramp', 2, 1000, {start=200}),
            tg.phase('steady', 4, 1000), tg.phase('cooldown', 2, 200)},
    classes={tg.http('http_keepalive', peer, 8888, {weight=8, keepalive=true}),
             tg.dns('dns', peer, 'snowtg.test', 1053, {weight=2})},
    assertions={tg.assertion('success_rate', '>=', .999),
                tg.assertion('latency_ms', '<', 20, {class_name='http_keepalive', phase='steady', quantile=.99}),
                tg.assertion('tx_alloc_fail', '==', 0),
                tg.assertion('drained_live_sockets', '==', 0)}})
