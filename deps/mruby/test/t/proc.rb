##
# Proc ISO Test

assert('Proc', '15.2.17') do
  Proc.class == Class
end

assert('Proc superclass', '15.2.17.2') do
  Proc.superclass == Object
end

assert('Proc.new', '15.2.17.3.1') do
  a = nil

  begin
    Proc.new
  rescue => e
    a = e
  end

  b = Proc.new {}

  a.class == ArgumentError and b.class == Proc
end

assert('Proc#[]', '15.2.17.4.1') do
  a = 0
  b = Proc.new { a += 1 }
  b.[]

  a2 = 0
  b2 = Proc.new { |i| a2 += i }
  b2.[](5)

  a == 1 and a2 == 5
end

assert('Proc#call', '15.2.17.4.3') do
  a = 0
  b = Proc.new { a += 1 }
  b.call

  a2 = 0
  b2 = Proc.new { |i| a2 += i }
  b2.call(5)

  a == 1 and a2 == 5 
end
