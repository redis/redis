##
# Bootstrap test for literals

assert('BS Literal 1') do
  true == true
end

assert('BS Literal 2') do
  TrueClass == true.class
end

assert('BS Literal 3') do
  false == false
end

assert('BS Literal 4') do
  FalseClass == false.class
end

assert('BS Literal 5') do
  'nil' == nil.inspect
end

assert('BS Literal 6') do
  NilClass == nil.class
end

assert('BS Literal 7') do
  Symbol == :sym.class
end

assert('BS Literal 8') do
  1234 == 1234
end

assert('BS Literal 9') do
  Fixnum == 1234.class
end
