local tg = require("snowtg")
return tg.scenario("redis-mixed", {duration=10, cps=100, concurrency=32, classes={
    tg.redis("ping", "198.18.0.2", nil, {command="PING"}),
    tg.redis("get", "198.18.0.2", nil, {command="GET", key="snowtg:key"}),
    tg.redis("set", "198.18.0.2", nil, {command="SET", key="snowtg:key", value="hello"}),
    tg.http("http", "198.18.0.2", 8080, {keepalive=true}),
    tg.dns("dns", "198.18.0.2", "snowtg.test", 1053),
}})
