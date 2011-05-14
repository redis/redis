-- Test cases for bit operations library. Public domain.

local bit = require"bit"

local vb = {
  0, 1, -1, 2, -2, 0x12345678, 0x87654321,
  0x33333333, 0x77777777, 0x55aa55aa, 0xaa55aa55,
  0x7fffffff, 0x80000000, 0xffffffff
}

local function cksum(name, s, r)
  local z = 0
  for i=1,#s do z = (z + string.byte(s, i)*i) % 2147483629 end
  if z ~= r then
    error("bit."..name.." test failed (got "..z..", expected "..r..")", 0)
  end
end

local function check_unop(name, r)
  local f = bit[name]
  local s = ""
  if pcall(f) or pcall(f, "z") or pcall(f, true) then
    error("bit."..name.." fails to detect argument errors", 0)
  end
  for _,x in ipairs(vb) do s = s..","..tostring(f(x)) end
  cksum(name, s, r)
end

local function check_binop(name, r)
  local f = bit[name]
  local s = ""
  if pcall(f) or pcall(f, "z") or pcall(f, true) then
    error("bit."..name.." fails to detect argument errors", 0)
  end
  for _,x in ipairs(vb) do
    for _,y in ipairs(vb) do s = s..","..tostring(f(x, y)) end
  end
  cksum(name, s, r)
end

local function check_binop_range(name, r, yb, ye)
  local f = bit[name]
  local s = ""
  if pcall(f) or pcall(f, "z") or pcall(f, true) or pcall(f, 1, true) then
    error("bit."..name.." fails to detect argument errors", 0)
  end
  for _,x in ipairs(vb) do
    for y=yb,ye do s = s..","..tostring(f(x, y)) end
  end
  cksum(name, s, r)
end

local function check_shift(name, r)
  check_binop_range(name, r, 0, 31)
end

-- Minimal sanity checks.
assert(0x7fffffff == 2147483647, "broken hex literals")
assert(0xffffffff == -1 or 0xffffffff == 2^32-1, "broken hex literals")
assert(tostring(-1) == "-1", "broken tostring()")
assert(tostring(0xffffffff) == "-1" or tostring(0xffffffff) == "4294967295", "broken tostring()")

-- Basic argument processing.
assert(bit.tobit(1) == 1)
assert(bit.band(1) == 1)
assert(bit.bxor(1,2) == 3)
assert(bit.bor(1,2,4,8,16,32,64,128) == 255)

-- Apply operations to test vectors and compare checksums.
check_unop("tobit", 277312)
check_unop("bnot", 287870)
check_unop("bswap", 307611)

check_binop("band", 41206764)
check_binop("bor", 51253663)
check_binop("bxor", 79322427)

check_shift("lshift", 325260344)
check_shift("rshift", 139061800)
check_shift("arshift", 111364720)
check_shift("rol", 302401155)
check_shift("ror", 302316761)

check_binop_range("tohex", 47880306, -8, 8)

