##
# Float ISO Test

assert('Float', '15.2.9') do
  Float.class == Class
end

assert('Float superclass', '15.2.9.2') do
  Float.superclass == Numeric
end

assert('Float#+', '15.2.9.3.1') do
  a = 3.123456788 + 0.000000001
  b = 3.123456789 + 1

  check_float(a, 3.123456789) and
    check_float(b, 4.123456789)
end

assert('Float#-', '15.2.9.3.2') do
  a = 3.123456790 - 0.000000001
  b = 5.123456789 - 1

  check_float(a, 3.123456789) and
    check_float(b, 4.123456789)
end

assert('Float#*', '15.2.9.3.3') do
  a = 3.125 * 3.125
  b = 3.125 * 1

  check_float(a, 9.765625) and
    check_float(b, 3.125)
end

assert('Float#/', '15.2.9.3.4') do
  a = 3.123456789 / 3.123456789
  b = 3.123456789 / 1

  check_float(a, 1.0) and
    check_float(b, 3.123456789)
end

assert('Float#%', '15.2.9.3.5') do
  a = 3.125 % 3.125
  b = 3.125 % 1

  check_float(a, 0.0) and
    check_float(b, 0.125)
end

assert('Float#<=>', '15.2.9.3.6') do
  a = 3.125 <=> 3.123
  b = 3.125 <=> 3.125
  c = 3.125 <=> 3.126
  a2 = 3.125 <=> 3
  c2 = 3.125 <=> 4

  a == 1 and b == 0 and c == -1 and
    a2 == 1 and c2 == -1
end

assert('Float#==', '15.2.9.3.7') do
  3.1 == 3.1 and not 3.1 == 3.2
end

assert('Float#ceil', '15.2.9.3.8') do
  a = 3.123456789.ceil
  b = 3.0.ceil
  c = -3.123456789.ceil
  d = -3.0.ceil
  a == 4 and b == 3 and c == -3 and d == -3
end

assert('Float#finite?', '15.2.9.3.9') do
  3.123456789.finite? and
    not (1.0 / 0.0).finite?
end

assert('Float#floor', '15.2.9.3.10') do
  a = 3.123456789.floor
  b = 3.0.floor
  c = -3.123456789.floor
  d = -3.0.floor
  a == 3 and b == 3 and c == -4 and d == -3
end

assert('Float#infinite?', '15.2.9.3.11') do
  a = 3.123456789.infinite?
  b = (1.0 / 0.0).infinite?
  c = (-1.0 / 0.0).infinite?

  a == nil and b == 1 and c == -1
end

assert('Float#round', '15.2.9.3.12') do
  a = 3.123456789.round
  b = 3.5.round
  c = 3.4999.round
  d = (-3.123456789).round
  e = (-3.5).round
  f = 12345.67.round(-1)
  g = 3.423456789.round(0)
  h = 3.423456789.round(1)
  i = 3.423456789.round(3)

  a == 3 and b == 4 and c == 3 and d == -3 and e == -4 and
    f == 12350 and g == 3 and check_float(h, 3.4) and check_float(i, 3.423)
end

assert('Float#to_f', '15.2.9.3.13') do
  a = 3.123456789

  check_float(a.to_f, a)
end

assert('Float#to_i', '15.2.9.3.14') do
  3.123456789.to_i == 3
end

assert('Float#truncate', '15.2.9.3.15') do
  3.123456789.truncate == 3 and -3.1.truncate == -3
end
