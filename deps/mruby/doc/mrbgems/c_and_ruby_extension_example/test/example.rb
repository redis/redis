assert('C and Ruby Extension Example 1') do
  CRubyExtension.respond_to? :c_method
end

assert('C and Ruby Extension Example 2') do
  CRubyExtension.respond_to? :ruby_method
end
