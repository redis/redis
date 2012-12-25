##
# RuntimeError ISO Test

assert('RuntimeError', '15.2.28') do
  RuntimeError.class == Class
end
