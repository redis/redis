##
# BasicObject

assert('BasicObject') do
  BasicObject.class == Class
end

assert('BasicObject superclass') do
  BasicObject.superclass == nil
end

