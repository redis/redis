##
# Literals ISO Test

assert('Literals Numerical', '8.7.6.2') do
  # signed and unsigned integer
  1 == 1 and -1 == -1 and +1 == +1 and
    # signed and unsigned float
    1.0 == 1.0 and -1.0 == -1.0 and
    # binary
    0b10000000 == 128 and 0B10000000 == 128
    # octal
    0o10 == 8 and 0O10 == 8 and 0_10 == 8
    # hex
    0xff == 255 and 0Xff == 255 and
    # decimal
    0d999 == 999 and 0D999 == 999 and
    # decimal seperator
    10_000_000 == 10000000 and 1_0 == 10 and
    # integer with exponent
    1e1 == 10.0 and 1e-1 == 0.1 and 1e+1 == 10.0
    # float with exponent
    1.0e1 == 10.0 and 1.0e-1 == 0.1 and 1.0e+1 == 10.0
end

assert('Literals Strings Single Quoted', '8.7.6.3.2') do
  'abc' == 'abc' and '\'' == '\'' and '\\' == '\\'
end

assert('Literals Strings Double Quoted', '8.7.6.3.3') do
  a = "abc"

  "abc" == "abc" and "\"" == "\"" and "\\" == "\\" and
    "#{a}" == "abc"
end

assert('Literals Strings Quoted Non-Expanded', '8.7.6.3.4') do
  a = %q{abc}
  b = %q(abc)
  c = %q[abc]
  d = %q<abc>
  e = %q/abc/
  f = %q/ab\/c/

  a == 'abc' and b == 'abc' and c == 'abc' and d == 'abc' and
    e == 'abc' and f == 'ab/c'
end

assert('Literals Strings Quoted Expanded', '8.7.6.3.5') do
  a = %Q{abc}
  b = %Q(abc)
  c = %Q[abc]
  d = %Q<abc>
  e = %Q/abc/
  f = %Q/ab\/c/
  g = %Q{#{a}}

  a == 'abc' and b == 'abc' and c == 'abc' and d == 'abc' and
    e == 'abc' and f == 'ab/c' and g == 'abc'
end

# Not Implemented ATM assert('Literals Strings Here documents', '8.7.6.3.6') do

# Not Implemented ATM assert('Literals Array', '8.7.6.4') do

# Not Implemented ATM assert('Literals Regular expression', '8.7.6.5') do

# Not Implemented ATM assert('Literals Symbol', '8.7.6.6') do
