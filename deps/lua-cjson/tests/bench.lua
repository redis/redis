#!/usr/bin/env lua

-- Simple JSON benchmark.
--
-- Your Mileage May Vary.
--
-- Mark Pulford <mark@kyne.com.au>

require "common"
local json = require "cjson"

function bench_file(filename)
    local data_json = file_load(filename)
    local data_obj = json.decode(data_json)

    local function test_encode ()
        json.encode(data_obj)
    end
    local function test_decode ()
        json.decode(data_json)
    end

    local tests = {
        encode = test_encode,
        decode = test_decode
    }

    return benchmark(tests, 5000, 5)
end

for i = 1, #arg do
    local results = bench_file(arg[i])
    for k, v in pairs(results) do
        print(string.format("%s: %s: %d", arg[i], k, v))
    end
end

-- vi:ai et sw=4 ts=4:
