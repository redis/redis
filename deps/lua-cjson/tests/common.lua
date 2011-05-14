require "cjson"
require "posix"

-- Misc routines to assist with CJSON testing
--
-- Mark Pulford <mark@kyne.com.au>

-- Determine with a Lua table can be treated as an array.
-- Explicitly returns "not an array" for very sparse arrays.
-- Returns:
-- -1   Not an array
-- 0    Empty table
-- >0   Highest index in the array
function is_array(table)
    local max = 0
    local count = 0
    for k, v in pairs(table) do
        if type(k) == "number" then
            if k > max then max = k end
            count = count + 1
        else
            return -1
        end
    end
    if max > count * 2 then
        return -1
    end

    return max
end

function serialise_table(value, indent, depth)
    local spacing, spacing2, indent2
    if indent then
        spacing = "\n" .. indent
        spacing2 = spacing .. "  "
        indent2 = indent .. "  "
    else
        spacing, spacing2, indent2 = " ", " ", false
    end
    depth = depth + 1
    if depth > 50 then
        return "Cannot serialise any further: too many nested tables"
    end

    local max = is_array(value)

    local comma = false
    local fragment = { "{" .. spacing2 }
    if max > 0 then
        -- Serialise array
        for i = 1, max do
            if comma then
                table.insert(fragment, "," .. spacing2)
            end
            table.insert(fragment, serialise_value(value[i], indent2, depth))
            comma = true
        end
    elseif max < 0 then
        -- Serialise table
        for k, v in pairs(value) do
            if comma then
                table.insert(fragment, "," .. spacing2)
            end
            table.insert(fragment, string.format(
                "[%s] = %s", serialise_value(k, indent2, depth),
                             serialise_value(v, indent2, depth))
            )
            comma = true
        end
    end
    table.insert(fragment, spacing .. "}")

    return table.concat(fragment)
end

function serialise_value(value, indent, depth)
    if indent == nil then indent = "" end
    if depth == nil then depth = 0 end

    if value == cjson.null then
        return "cjson.null"
    elseif type(value) == "string" then
        return string.format("%q", value)
    elseif type(value) == "nil" or type(value) == "number" or
           type(value) == "boolean" then
        return tostring(value)
    elseif type(value) == "table" then
        return serialise_table(value, indent, depth)
    else
        return "\"<" .. type(value) .. ">\""
    end
end

function dump_value(value)
    print(serialise_value(value))
end

function file_load(filename)
    local file, err = io.open(filename)
    if file == nil then
        error("Unable to read " .. filename)
    end
    local data = file:read("*a")
    file:close()

    if data == nil then
        error("Failed to read " .. filename)
    end

    return data
end

function file_save(filename, data)
    local file, err = io.open(filename, "w")
    if file == nil then
        error("Unable to write " .. filename)
    end
    file:write(data)
    file:close()
end

function gettimeofday()
    local tv_sec, tv_usec = posix.gettimeofday()

    return tv_sec + tv_usec / 1000000
end

function benchmark(tests, iter, rep)
    local function bench(func, iter)
        collectgarbage("stop")
        local t = gettimeofday()
        for i = 1, iter do
            func(i)
        end
        t = gettimeofday() - t
        collectgarbage("restart")
        return (iter / t)
    end

    local test_results = {}
    for name, func in pairs(tests) do
        -- k(number), v(string)
        -- k(string), v(function)
        -- k(number), v(function)
        if type(func) == "string" then
            name = func
            func = _G[name]
        end
        local result = {}
        for i = 1, rep do
            result[i] = bench(func, iter)
        end
        table.sort(result)
        test_results[name] = result[rep]
    end

    return test_results
end

function compare_values(val1, val2)
    local type1 = type(val1)
    local type2 = type(val2)
    if type1 ~= type2 then
        return false
    end

    -- Check for NaN
    if type1 == "number" and val1 ~= val1 and val2 ~= val2 then
        return true
    end

    if type1 ~= "table" then
        return val1 == val2
    end

    -- check_keys stores all the keys that must be checked in val2
    local check_keys = {}
    for k, _ in pairs(val1) do
        check_keys[k] = true
    end

    for k, v in pairs(val2) do
        if not check_keys[k] then
            return false
        end

        if not compare_values(val1[k], val2[k]) then
            return false
        end

        check_keys[k] = nil
    end
    for k, _ in pairs(check_keys) do
        -- Not the same if any keys from val1 were not found in val2
        return false
    end
    return true
end

function run_test(testname, func, input, should_work, output)
    local function status_line(name, status, value)
        local statusmap = { [true] = ":success", [false] = ":error" }
        if status ~= nil then
            name = name .. statusmap[status]
        end
        print(string.format("[%s] %s", name, serialise_value(value, false)))
    end

    local result = { pcall(func, unpack(input)) }
    local success = table.remove(result, 1)

    local correct = false
    if success == should_work and compare_values(result, output) then
        correct = true
    end

    local teststatus = { [true] = "PASS", [false] = "FAIL" }
    print("==> Test " .. testname .. ": " .. teststatus[correct])

    status_line("Input", nil, input)
    if not correct then
        status_line("Expected", should_work, output)
    end
    status_line("Received", success, result)
    print()

    return correct, result
end

function run_test_group(testgroup, tests)
    local function run_config(configname, func)
        local success, msg = pcall(func)
        print(string.format("==> Config %s: %s", configname, msg))
        print()
    end

    local function test_id(group, id)
        return string.format("%s [%d]", group, id)
    end

    for k, v in ipairs(tests) do
        if type(v) == "function" then
            -- Useful for changing configuration during a batch
            run_config(test_id(testgroup, k), v)
        elseif type(v) == "table" then
            run_test(test_id(testgroup, k), unpack(v))
        else
            error("Testgroup can only contain functions and tables")
        end
    end
end

-- vi:ai et sw=4 ts=4:
