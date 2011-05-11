-- This is the (naive) Sieve of Eratosthenes. Public domain.

local bit = require("bit")
local band, bxor, rshift, rol = bit.band, bit.bxor, bit.rshift, bit.rol

local function nsieve(p, m)
  local count = 0
  for i=0,(m+31)/32 do p[i] = -1 end
  for i=2,m do
    if band(rshift(p[rshift(i, 5)], i), 1) ~= 0 then
      count = count + 1
      for j=i+i,m,i do
	local jx = rshift(j, 5)
	p[jx] = band(p[jx], rol(-2, j))
      end
    end
  end
  return count
end

if arg and arg[1] then
  local N = tonumber(arg[1]) or 1
  if N < 2 then N = 2 end
  local primes = {}

  for i=0,2 do
    local m = (2^(N-i))*10000
    io.write(string.format("Primes up to %8d %8d\n", m, nsieve(primes, m)))
  end
else
  assert(nsieve({}, 10000) == 1229)
end
