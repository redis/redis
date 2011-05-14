-- MD5 test and benchmark. Public domain.

local bit = require("bit")
local tobit, tohex, bnot = bit.tobit or bit.cast, bit.tohex, bit.bnot
local bor, band, bxor = bit.bor, bit.band, bit.bxor
local lshift, rshift, rol, bswap = bit.lshift, bit.rshift, bit.rol, bit.bswap
local byte, char, sub, rep = string.byte, string.char, string.sub, string.rep

if not rol then -- Replacement function if rotates are missing.
  local bor, shl, shr = bit.bor, bit.lshift, bit.rshift
  function rol(a, b) return bor(shl(a, b), shr(a, 32-b)) end
end

if not bswap then -- Replacement function if bswap is missing.
  local bor, band, shl, shr = bit.bor, bit.band, bit.lshift, bit.rshift
  function bswap(a)
    return bor(shr(a, 24), band(shr(a, 8), 0xff00),
	       shl(band(a, 0xff00), 8), shl(a, 24));
  end
end

if not tohex then -- (Unreliable) replacement function if tohex is missing.
  function tohex(a)
    return string.sub(string.format("%08x", a), -8)
  end
end

local function tr_f(a, b, c, d, x, s)
  return rol(bxor(d, band(b, bxor(c, d))) + a + x, s) + b
end

local function tr_g(a, b, c, d, x, s)
  return rol(bxor(c, band(d, bxor(b, c))) + a + x, s) + b
end

local function tr_h(a, b, c, d, x, s)
  return rol(bxor(b, c, d) + a + x, s) + b
end

local function tr_i(a, b, c, d, x, s)
  return rol(bxor(c, bor(b, bnot(d))) + a + x, s) + b
end

local function transform(x, a1, b1, c1, d1)
  local a, b, c, d = a1, b1, c1, d1

  a = tr_f(a, b, c, d, x[ 1] + 0xd76aa478,  7)
  d = tr_f(d, a, b, c, x[ 2] + 0xe8c7b756, 12)
  c = tr_f(c, d, a, b, x[ 3] + 0x242070db, 17)
  b = tr_f(b, c, d, a, x[ 4] + 0xc1bdceee, 22)
  a = tr_f(a, b, c, d, x[ 5] + 0xf57c0faf,  7)
  d = tr_f(d, a, b, c, x[ 6] + 0x4787c62a, 12)
  c = tr_f(c, d, a, b, x[ 7] + 0xa8304613, 17)
  b = tr_f(b, c, d, a, x[ 8] + 0xfd469501, 22)
  a = tr_f(a, b, c, d, x[ 9] + 0x698098d8,  7)
  d = tr_f(d, a, b, c, x[10] + 0x8b44f7af, 12)
  c = tr_f(c, d, a, b, x[11] + 0xffff5bb1, 17)
  b = tr_f(b, c, d, a, x[12] + 0x895cd7be, 22)
  a = tr_f(a, b, c, d, x[13] + 0x6b901122,  7)
  d = tr_f(d, a, b, c, x[14] + 0xfd987193, 12)
  c = tr_f(c, d, a, b, x[15] + 0xa679438e, 17)
  b = tr_f(b, c, d, a, x[16] + 0x49b40821, 22)

  a = tr_g(a, b, c, d, x[ 2] + 0xf61e2562,  5)
  d = tr_g(d, a, b, c, x[ 7] + 0xc040b340,  9)
  c = tr_g(c, d, a, b, x[12] + 0x265e5a51, 14)
  b = tr_g(b, c, d, a, x[ 1] + 0xe9b6c7aa, 20)
  a = tr_g(a, b, c, d, x[ 6] + 0xd62f105d,  5)
  d = tr_g(d, a, b, c, x[11] + 0x02441453,  9)
  c = tr_g(c, d, a, b, x[16] + 0xd8a1e681, 14)
  b = tr_g(b, c, d, a, x[ 5] + 0xe7d3fbc8, 20)
  a = tr_g(a, b, c, d, x[10] + 0x21e1cde6,  5)
  d = tr_g(d, a, b, c, x[15] + 0xc33707d6,  9)
  c = tr_g(c, d, a, b, x[ 4] + 0xf4d50d87, 14)
  b = tr_g(b, c, d, a, x[ 9] + 0x455a14ed, 20)
  a = tr_g(a, b, c, d, x[14] + 0xa9e3e905,  5)
  d = tr_g(d, a, b, c, x[ 3] + 0xfcefa3f8,  9)
  c = tr_g(c, d, a, b, x[ 8] + 0x676f02d9, 14)
  b = tr_g(b, c, d, a, x[13] + 0x8d2a4c8a, 20)

  a = tr_h(a, b, c, d, x[ 6] + 0xfffa3942,  4)
  d = tr_h(d, a, b, c, x[ 9] + 0x8771f681, 11)
  c = tr_h(c, d, a, b, x[12] + 0x6d9d6122, 16)
  b = tr_h(b, c, d, a, x[15] + 0xfde5380c, 23)
  a = tr_h(a, b, c, d, x[ 2] + 0xa4beea44,  4)
  d = tr_h(d, a, b, c, x[ 5] + 0x4bdecfa9, 11)
  c = tr_h(c, d, a, b, x[ 8] + 0xf6bb4b60, 16)
  b = tr_h(b, c, d, a, x[11] + 0xbebfbc70, 23)
  a = tr_h(a, b, c, d, x[14] + 0x289b7ec6,  4)
  d = tr_h(d, a, b, c, x[ 1] + 0xeaa127fa, 11)
  c = tr_h(c, d, a, b, x[ 4] + 0xd4ef3085, 16)
  b = tr_h(b, c, d, a, x[ 7] + 0x04881d05, 23)
  a = tr_h(a, b, c, d, x[10] + 0xd9d4d039,  4)
  d = tr_h(d, a, b, c, x[13] + 0xe6db99e5, 11)
  c = tr_h(c, d, a, b, x[16] + 0x1fa27cf8, 16)
  b = tr_h(b, c, d, a, x[ 3] + 0xc4ac5665, 23)

  a = tr_i(a, b, c, d, x[ 1] + 0xf4292244,  6)
  d = tr_i(d, a, b, c, x[ 8] + 0x432aff97, 10)
  c = tr_i(c, d, a, b, x[15] + 0xab9423a7, 15)
  b = tr_i(b, c, d, a, x[ 6] + 0xfc93a039, 21)
  a = tr_i(a, b, c, d, x[13] + 0x655b59c3,  6)
  d = tr_i(d, a, b, c, x[ 4] + 0x8f0ccc92, 10)
  c = tr_i(c, d, a, b, x[11] + 0xffeff47d, 15)
  b = tr_i(b, c, d, a, x[ 2] + 0x85845dd1, 21)
  a = tr_i(a, b, c, d, x[ 9] + 0x6fa87e4f,  6)
  d = tr_i(d, a, b, c, x[16] + 0xfe2ce6e0, 10)
  c = tr_i(c, d, a, b, x[ 7] + 0xa3014314, 15)
  b = tr_i(b, c, d, a, x[14] + 0x4e0811a1, 21)
  a = tr_i(a, b, c, d, x[ 5] + 0xf7537e82,  6)
  d = tr_i(d, a, b, c, x[12] + 0xbd3af235, 10)
  c = tr_i(c, d, a, b, x[ 3] + 0x2ad7d2bb, 15)
  b = tr_i(b, c, d, a, x[10] + 0xeb86d391, 21)

  return tobit(a+a1), tobit(b+b1), tobit(c+c1), tobit(d+d1)
end

-- Note: this is copying the original string and NOT particularly fast.
-- A library for struct unpacking would make this task much easier.
local function md5(msg)
  local len = #msg
  msg = msg.."\128"..rep("\0", 63 - band(len + 8, 63))
	   ..char(band(lshift(len, 3), 255), band(rshift(len, 5), 255),
		  band(rshift(len, 13), 255), band(rshift(len, 21), 255))
	   .."\0\0\0\0"
  local a, b, c, d = 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476
  local x, k = {}, 1
  for i=1,#msg,4 do
    local m0, m1, m2, m3 = byte(msg, i, i+3)
    x[k] = bor(m0, lshift(m1, 8), lshift(m2, 16), lshift(m3, 24))
    if k == 16 then
      a, b, c, d = transform(x, a, b, c, d)
      k = 1
    else
      k = k + 1
    end
  end
  return tohex(bswap(a))..tohex(bswap(b))..tohex(bswap(c))..tohex(bswap(d))
end

assert(md5('') == 'd41d8cd98f00b204e9800998ecf8427e')
assert(md5('a') == '0cc175b9c0f1b6a831c399e269772661')
assert(md5('abc') == '900150983cd24fb0d6963f7d28e17f72')
assert(md5('message digest') == 'f96b697d7cb7938d525a2f31aaf161d0')
assert(md5('abcdefghijklmnopqrstuvwxyz') == 'c3fcd3d76192e4007dfb496cca67e13b')
assert(md5('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789') ==
       'd174ab98d277d9f5a5611c2c9f419d9f')
assert(md5('12345678901234567890123456789012345678901234567890123456789012345678901234567890') ==
       '57edf4a22be3c955ac49da2e2107b67a')

if arg and arg[1] == "bench" then
  -- Credits: William Shakespeare, Romeo and Juliet
  local txt = [[Rebellious subjects, enemies to peace,
Profaners of this neighbour-stained steel,--
Will they not hear? What, ho! you men, you beasts,
That quench the fire of your pernicious rage
With purple fountains issuing from your veins,
On pain of torture, from those bloody hands
Throw your mistemper'd weapons to the ground,
And hear the sentence of your moved prince.
Three civil brawls, bred of an airy word,
By thee, old Capulet, and Montague,
Have thrice disturb'd the quiet of our streets,
And made Verona's ancient citizens
Cast by their grave beseeming ornaments,
To wield old partisans, in hands as old,
Canker'd with peace, to part your canker'd hate:
If ever you disturb our streets again,
Your lives shall pay the forfeit of the peace.
For this time, all the rest depart away:
You Capulet; shall go along with me:
And, Montague, come you this afternoon,
To know our further pleasure in this case,
To old Free-town, our common judgment-place.
Once more, on pain of death, all men depart.]]
  txt = txt..txt..txt..txt
  txt = txt..txt..txt..txt

  local function bench()
    local n = 80
    repeat
      local tm = os.clock()
      local res
      for i=1,n do
	res = md5(txt)
      end
      assert(res == 'a831e91e0f70eddcb70dc61c6f82f6cd')
      tm = os.clock() - tm
      if tm > 1 then return tm*(1000000000/(n*#txt)) end
      n = n + n
    until false
  end

  io.write(string.format("MD5 %7.1f ns/char\n", bench()))
end

