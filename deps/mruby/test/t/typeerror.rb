##
# TypeError ISO Test

assert('TypeError', '15.2.29') do
  TypeError.class == Class
end

assert('TypeError superclass', '15.2.29.2') do
  TypeError.superclass == StandardError
end

