##
# ArgumentError ISO Test

assert('ArgumentError', '15.2.24') do
  e2 = nil
  a = []
  begin
    # this will cause an exception due to the wrong arguments
    a[]
  rescue => e1
    e2 = e1
  end

  ArgumentError.class == Class and e2.class == ArgumentError
end

assert('ArgumentError superclass', '15.2.24.2') do
  ArgumentError.superclass == StandardError
end

