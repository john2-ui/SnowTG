-- Equivalent to the Python/JSON example; no Lua executes in the packet path.
local tg=require('snowtg')
local peer=os.getenv('SNOWTG_PEER') or '192.168.10.234'
return tg.scenario('business-http-dns', {duration=5,cps=20,concurrency=16,seed=17,
    classes={tg.transaction('business',{vars={peer=peer},dataset='workflow-users.csv',steps={
        -- Copy response scalars into ctx; Header index=1 selects the second occurrence.
        tg.dns_step('resolve','${vars.peer}','alias.snowtg.test',15353,{extract={address=tg.extract('address')}}),
        tg.http_step('fetch','${ctx.address}',18080,{path='/chunked',keepalive=true,
            extract={token=tg.extract('json','/token'),['repeat']=tg.extract('header','x-repeat',{index=1})},
            checks={tg.check(tg.extract('status'),'==',200),tg.check(tg.extract('json','/items/0'),'==',7)}}),
        -- Only describe control flow here; native execution chooses the dataset row and wait.
        tg.think('pause',nil,{minimum=3,maximum=8}),
        tg.branch('route',tg.check(tg.ref('data.route'),'==','A'),{['then']='route_a',otherwise='route_b'}),
        -- The json filter quotes its value; next skips the other path and joins at finish.
        tg.http_step('route_a','${ctx.address}',18080,{path='/use',method='POST',keepalive=true,
            headers={['X-Route']='A'},body='{"token":${ctx.token|json},"user":${data.user|json}}',next='finish'}),
        tg.http_step('route_b','${ctx.address}',18080,{path='/use',method='POST',keepalive=true,
            headers={['X-Route']='B'},body='{"token":${ctx.token|json},"user":${data.user|json}}'}),
        tg.http_step('finish','${ctx.address}',18080,{path='/close'})}})},
    -- Separate whole-business acceptance from fetch-step latency and final socket drain.
    assertions={tg.assertion('success_rate','==',1),tg.assertion('success_rate','==',1,{step='fetch'}),
        tg.assertion('latency_ms','<',100,{quantile=.99,step='fetch'}),tg.assertion('drained_live_sockets','==',0)}})
