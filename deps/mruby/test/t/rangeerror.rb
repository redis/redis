##
# RangeError ISO Test

assert('RangeError', '15.2.26') do
  RangeError.class == Class
end

assert('RangeError superclass', '15.2.26.2') do
  RangeError.superclass == StandardError
end

