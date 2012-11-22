##
# IndexError ISO Test

assert('IndexError', '15.2.33') do
  IndexError.class == Class
end

assert('IndexError superclass', '15.2.33.2') do
  IndexError.superclass == StandardError
end

