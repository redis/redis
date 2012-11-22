##
# Math Test

if Object.const_defined?(:Math)
  assert('Math.sin 0') do
    check_float(Math.sin(0), 0)
  end

  assert('Math.sin PI/2') do
    check_float(Math.sin(Math::PI / 2), 1)
  end

  assert('Fundamental trig identities') do
    result = true
    N = 13
    N.times do |i|
      a  = Math::PI / N * i
      ca = Math::PI / 2 - a
      s  = Math.sin(a)
      c  = Math.cos(a)
      t  = Math.tan(a)
      result &= check_float(s, Math.cos(ca))
      result &= check_float(t, 1 / Math.tan(ca))
      result &= check_float(s ** 2 + c ** 2, 1)
      result &= check_float(t ** 2 + 1, (1/c) ** 2)
      result &= check_float((1/t) ** 2 + 1, (1/s) ** 2)
    end
    result
  end

  assert('Math.erf 0') do
    check_float(Math.erf(0), 0)
  end

  assert('Math.exp 0') do
    check_float(Math.exp(0), 1.0)
  end

  assert('Math.exp 1') do
    check_float(Math.exp(1), 2.718281828459045)
  end

  assert('Math.exp 1.5') do
    check_float(Math.exp(1.5), 4.4816890703380645)
  end

  assert('Math.log 1') do
    check_float(Math.log(1), 0)
  end

  assert('Math.log E') do
    check_float(Math.log(Math::E), 1.0)
  end

  assert('Math.log E**3') do
    check_float(Math.log(Math::E**3), 3.0)
  end

  assert('Math.log2 1') do
    check_float(Math.log2(1), 0.0)
  end

  assert('Math.log2 2') do
    check_float(Math.log2(2), 1.0)
  end

  assert('Math.log10 1') do
    check_float(Math.log10(1), 0.0)
  end

  assert('Math.log10 10') do
    check_float(Math.log10(10), 1.0)
  end

  assert('Math.log10 10**100') do
    check_float(Math.log10(10**100), 100.0)
  end

  assert('Math.sqrt') do
    num = [0.0, 1.0, 2.0, 3.0, 4.0]
    sqr = [0, 1, 4, 9, 16]
    result = true
    sqr.each_with_index do |v,i|
      result &= check_float(Math.sqrt(v), num[i])
    end
    result
  end

  assert('Math.cbrt') do
    num = [-2.0, -1.0, 0.0, 1.0, 2.0]
    cub = [-8, -1, 0, 1, 8]
    result = true
    cub.each_with_index do |v,i|
      result &= check_float(Math.cbrt(v), num[i])
    end
    result
  end

  assert('Math.hypot') do
    check_float(Math.hypot(3, 4), 5.0)
  end

  assert('Math.frexp 1234') do
    n = 1234
    fraction, exponent = Math.frexp(n)
    check_float(Math.ldexp(fraction, exponent), n)
  end

  assert('Math.erf 1') do
    check_float(Math.erf(1), 0.842700792949715)
  end

  assert('Math.erfc 1') do
    check_float(Math.erfc(1), 0.157299207050285)
  end

  assert('Math.erf -1') do
    check_float(Math.erf(-1), -0.8427007929497148)
  end

  assert('Math.erfc -1') do
    check_float(Math.erfc(-1), 1.8427007929497148)
  end
end

