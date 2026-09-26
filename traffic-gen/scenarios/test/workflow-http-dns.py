"""Business TPS: DNS -> JSON/Header capture -> think/branch -> POST -> close.
Run with snowtg.py run; SNOWTG_PEER points at test/workflow_peer.py's host.
Dataset rows are immutable and cycle by planned transaction ordinal.
"""
import os
from snowtg import (scenario, transaction, dns_step, http_step, think, branch,
                    extract, ref, check, assertion)
peer = os.environ.get('SNOWTG_PEER', '192.168.10.234')
plan = scenario('business-http-dns', duration=5, cps=20, concurrency=16, seed=17,
    classes=[transaction('business', vars={'peer': peer}, dataset='workflow-users.csv', steps=[
        # Save the resolved address in private ctx for later dynamic HTTP targets.
        dns_step('resolve', '${vars.peer}', 'alias.snowtg.test', 15353,
                 extract={'address': extract('address')}),
        http_step('fetch', '${ctx.address}', 18080, path='/chunked', keepalive=True,
                  extract={'token': extract('json', '/token'), 'repeat': extract('header', 'x-repeat', index=1)},
                  checks=[check(extract('status'), '==', 200), check(extract('json', '/items/0'), '==', 7)]),
        # Native seed/ordinal/step sampling controls this wait, not Python random or sleep.
        think('pause', minimum=3, maximum=8),
        branch('route', check(ref('data.route'), '==', 'A'), then='route_a', otherwise='route_b'),
        # The json filter includes quotes. Jump to finish to avoid executing route_b too.
        http_step('route_a', '${ctx.address}', 18080, path='/use', method='POST', keepalive=True,
                  headers={'X-Route': 'A'}, body='{"token":${ctx.token|json},"user":${data.user|json}}', next='finish'),
        http_step('route_b', '${ctx.address}', 18080, path='/use', method='POST', keepalive=True,
                  headers={'X-Route': 'B'}, body='{"token":${ctx.token|json},"user":${data.user|json}}'),
        http_step('finish', '${ctx.address}', 18080, path='/close')])],
    # Unscoped SLOs cover the business; fetch latency measures that network step only.
    assertions=[assertion('success_rate', '==', 1),
                assertion('success_rate', '==', 1, step='fetch'),
                assertion('latency_ms', '<', 100, quantile=.99, step='fetch'),
                assertion('drained_live_sockets', '==', 0)])
