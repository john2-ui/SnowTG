-- Startup-only scenario helpers. The Python launcher runs this bridge once.
assert(math.type, "Lua 5.3 or newer required")
local M = {}

local function checked(options, allowed)
    if options == nil then options = {} end
    assert(type(options) == "table", "options must be a table")
    local names = {}
    for name in allowed:gmatch("%S+") do names[name] = true end
    for key in pairs(options) do
        assert(names[key], "unknown option: " .. tostring(key))
    end
    return options
end

local function default(value, fallback)
    if value == nil then return fallback end
    return value
end

-- Rates count logical transactions (including Keep-Alive reuse), not sockets.
-- Duration is seconds; options.start selects a linear ramp to cps.
function M.phase(name, duration, cps, options)
    options = checked(options, "start")
    return {name=name, duration_sec=duration, target_cps=cps, start_cps=options.start}
end

function M.http(name, ip, port, options)
    options = checked(options, "weight path keepalive method host")
    local keepalive = options.keepalive
    if keepalive == nil then keepalive = false end
    return {name=name, weight=default(options.weight, 1), transport="tcp",
        peer={ip=ip, port=default(port, 80)},
        http={method=default(options.method, "GET"), path=default(options.path, "/"),
              host=options.host, keepalive=keepalive}}
end

function M.dns(name, ip, qname, port, options)
    options = checked(options, "weight qtype")
    return {name=name, weight=default(options.weight, 1), transport="udp",
        peer={ip=ip, port=default(port, 53)}, dns={qname=qname, qtype=default(options.qtype, "A")}}
end

-- Concurrency is global. Use phases OR duration/cps; native validation checks
-- protocol/rate limits after the launcher removes managed-run metadata.
function M.scenario(name, options)
    options = checked(options, "classes concurrency report_interval phases duration cps assertions purpose service")
    local result = {name=name, load_model="open", classes=options.classes,
        assertions=options.assertions, purpose=options.purpose, service=options.service,
        max_concurrency=default(options.concurrency, 256),
        report_interval_sec=default(options.report_interval, 1)}
    if options.phases ~= nil then
        assert(options.duration == nil and options.cps == nil,
               "use phases OR duration/cps, not both")
        result.phases = options.phases
    else
        assert(options.duration ~= nil and options.cps ~= nil,
               "provide phases or both duration and cps")
        result.duration_sec, result.target_cps = options.duration, options.cps
    end
    return result
end

-- Ratios use [0,1], latency uses ms and quantile in (0,1]. Selectors/thresholds
-- are validated by the managed runner; critical failures yield exit code 2.
function M.assertion(metric, op, value, options)
    options = checked(options, "class_name protocol phase quantile latency_metric critical")
    return {metric=metric, op=op, value=value, class=options.class_name,
        protocol=options.protocol, phase=options.phase, quantile=options.quantile,
        latency_metric=options.latency_metric, critical=default(options.critical, true)}
end

if ... == "snowtg" then return M end

-- ponytail: serialize only JSON-compatible Lua values; no JSON package needed.
-- Empty tables are arrays; mixed/sparse tables, cycles and non-finite numbers
-- are errors, never silently dropped. Schema validation stays in the C loader.
local function encode(value, seen)
    local kind = type(value)
    if kind == "string" then
        return '"' .. value:gsub('[%z\1-\31\\"]', function(c)
            return string.format('\\u%04x', c:byte())
        end) .. '"'
    elseif kind == "boolean" then
        return tostring(value)
    elseif kind == "number" then
        assert(value == value and value ~= math.huge and value ~= -math.huge,
               "non-finite number in plan")
        if math.type(value) == "integer" then return tostring(value) end
        if value == math.floor(value) then return string.format("%.0f", value) end
        return string.format("%.17g", value)
    end
    assert(kind == "table", "unsupported plan value: " .. kind)
    assert(not seen[value], "cyclic table in plan")
    seen[value] = true
    local keys, array, count = {}, true, 0
    for key in pairs(value) do
        count = count + 1
        keys[count] = key
        if type(key) ~= "number" then array = false end
    end
    local parts = {}
    if array then
        for _, key in ipairs(keys) do
            assert(key >= 1 and key <= count and key == math.floor(key),
                   "sparse/non-sequence table in plan")
        end
        for i = 1, count do parts[i] = encode(value[i], seen) end
    else
        for _, key in ipairs(keys) do
            assert(type(key) == "string", "mixed/non-string keys in plan")
        end
        table.sort(keys)
        for i, key in ipairs(keys) do
            parts[i] = encode(key, seen) .. ":" .. encode(value[key], seen)
        end
    end
    seen[value] = nil
    return (array and "[" or "{") .. table.concat(parts, ",") .. (array and "]" or "}")
end

-- Execute trusted user code once with sibling imports and script-only argv.
-- A returned table takes precedence over the optional global plan variable.
local script, output_path = arg[1], arg[2]
package.loaded.snowtg = M
local directory = assert(script:match("^(.*)/"))
package.path = directory .. "/?.lua;" .. directory .. "/?/init.lua;" .. package.path
arg = {[0]=script}
local result = dofile(script)
if result == nil then result = _G.plan end
assert(type(result) == "table", "script must return a table or export 'plan'")
local text = encode(result, {}) .. "\n"
local output = assert(io.open(output_path, "w"))
assert(output:write(text))
assert(output:close())
