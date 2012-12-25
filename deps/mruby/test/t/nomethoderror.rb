##
# NoMethodError ISO Test

assert('NoMethodError', '15.2.32') do
  e2 = nil
  begin
    doesNotExistAsAMethodNameForVerySure("")
  rescue => e1
    e2 = e1
  end

  NoMethodError.class == Class and e2.class == NoMethodError
end

assert('NoMethodError superclass', '15.2.32.2') do
  NoMethodError.superclass == NameError
end

