assert('C Extension Example') do
  CExtension.respond_to? :c_method
end
