##
# FalseClass ISO Test

assert('FalseClass', '15.2.6') do
  FalseClass.class == Class
end

assert('FalseClass superclass', '15.2.6.2') do
  FalseClass.superclass == Object
end

assert('FalseClass false', '15.2.6.1') do
  not false
end

assert('FalseClass#&', '15.2.6.3.1') do
  not false.&(true) and not false.&(false)
end

assert('FalseClass#^', '15.2.6.3.2') do
  false.^(true) and not false.^(false)
end

assert('FalseClass#to_s', '15.2.6.3.3') do
  false.to_s == 'false'
end

assert('FalseClass#|', '15.2.6.3.4') do
  false.|(true) and not false.|(false)
end
