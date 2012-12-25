##
# Symbol ISO Test

assert('Symbol', '15.2.11') do
  Symbol.class == Class
end

assert('Symbol superclass', '15.2.11.2') do
  Symbol.superclass == Object
end

assert('Symbol#===', '15.2.11.3.1') do
  :abc === :abc and not :abc === :cba
end

assert('Symbol#id2name', '15.2.11.3.2') do
  :abc.id2name == 'abc'
end

assert('Symbol#to_s', '15.2.11.3.3') do
  :abc.to_s == 'abc'
end

assert('Symbol#to_sym', '15.2.11.3.4') do
  :abc.to_sym == :abc
end
