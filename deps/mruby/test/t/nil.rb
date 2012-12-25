##
# NilClass ISO Test

assert('NilClass', '15.2.4') do
  NilClass.class == Class
end

assert('NilClass#&', '15.2.4.3.1') do
  not nil.&(true) and not nil.&(nil)
end

assert('NilClass#^', '15.2.4.3.2') do
  nil.^(true) and not nil.^(false)
end

assert('NilClass#|', '15.2.4.3.3') do
  nil.|(true) and not nil.|(false)
end

assert('NilClass#nil?', '15.2.4.3.4') do
  nil.nil?
end

assert('NilClass#to_s', '15.2.4.3.5') do
  nil.to_s == ''
end
