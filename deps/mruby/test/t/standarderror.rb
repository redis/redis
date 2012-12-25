##
# StandardError ISO Test

assert('StandardError', '15.2.23') do
  StandardError.class == Class
end

assert('StandardError superclass', '15.2.23.2') do
  StandardError.superclass == Exception
end

