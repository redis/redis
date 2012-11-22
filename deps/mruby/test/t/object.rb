##
# Object ISO Test

assert('Object', '15.2.1') do
  Object.class == Class
end

assert('Object superclass', '15.2.1.2') do
  Object.superclass == BasicObject
end

