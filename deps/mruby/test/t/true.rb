##
# TrueClass ISO Test

assert('TrueClass', '15.2.5') do
  TrueClass.class == Class
end

assert('TrueClass superclass', '15.2.5.2') do
  TrueClass.superclass == Object
end

assert('TrueClass true', '15.2.5.1') do
  true
end

assert('TrueClass#&', '15.2.5.3.1') do
  true.&(true) and not true.&(false)
end

assert('TrueClass#^', '15.2.5.3.2') do
  not true.^(true) and true.^(false)
end

assert('TrueClass#to_s', '15.2.5.3.3') do
  true.to_s == 'true'
end

assert('TrueClass#|', '15.2.5.3.4') do
  true.|(true) and true.|(false)
end
