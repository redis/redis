##
# NameError ISO Test

assert('NameError', '15.2.31') do
  NameError.class == Class
end

assert('NameError superclass', '15.2.31.2') do
  NameError.superclass == StandardError
end

# TODO 15.2.31.2.1 NameError#name

assert('NameError#initialize', '15.2.31.2.2') do
   e = NameError.new.initialize('a')

   e.class == NameError and e.message == 'a'
end
