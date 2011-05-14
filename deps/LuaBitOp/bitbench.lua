-- Microbenchmark for bit operations library. Public domain.

local bit = require"bit"

if not bit.rol then -- Replacement function if rotates are missing.
  local bor, shl, shr = bit.bor, bit.lshift, bit.rshift
  function bit.rol(a, b) return bor(shl(a, b), shr(a, 32-b)) end
end

if not bit.bswap then -- Replacement function if bswap is missing.
  local bor, band, shl, shr = bit.bor, bit.band, bit.lshift, bit.rshift
  function bit.bswap(a)
    return bor(shr(a, 24), band(shr(a, 8), 0xff00),
	       shl(band(a, 0xff00), 8), shl(a, 24));
  end
end

local base = 0

local function bench(name, t)
  local n = 2000000
  repeat
    local tm = os.clock()
    t(n)
    tm = os.clock() - tm
    if tm > 1 then
      local ns = tm*1000/(n/1000000)
      io.write(string.format("%-15s %6.1f ns\n", name, ns-base))
      return ns
    end
    n = n + n
  until false
end

-- The overhead for the base loop is subtracted from the other measurements.
base = bench("loop baseline", function(n)
  local x = 0; for i=1,n do x = x + i end
end)

bench("tobit", function(n)
  local f = bit.tobit or bit.cast
  local x = 0; for i=1,n do x = x + f(i) end
end)

bench("bnot", function(n)
  local f = bit.bnot
  local x = 0; for i=1,n do x = x + f(i) end
end)

bench("bor/band/bxor", function(n)
  local f = bit.bor
  local x = 0; for i=1,n do x = x + f(i, 1) end
end)

bench("shifts", function(n)
  local f = bit.lshift
  local x = 0; for i=1,n do x = x + f(i, 1) end
end)

bench("rotates", function(n)
  local f = bit.rol
  local x = 0; for i=1,n do x = x + f(i, 1) end
end)

bench("bswap", function(n)
  local f = bit.bswap
  local x = 0; for i=1,n do x = x + f(i) end
end)

