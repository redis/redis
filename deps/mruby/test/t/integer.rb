##
# Integer ISO Test

assert('Integer', '15.2.8') do
  Integer.class == Class
end

assert('Integer superclass', '15.2.8.2') do
  Integer.superclass == Numeric
end

assert('Integer#+', '15.2.8.3.1') do
  a = 1+1
  b = 1+1.0

  a == 2 and b == 2.0
end

assert('Integer#-', '15.2.8.3.2') do
  a = 2-1
  b = 2-1.0

  a == 1 and b == 1.0
end

assert('Integer#*', '15.2.8.3.3') do
  a = 1*1
  b = 1*1.0

  a == 1 and b == 1.0
end

assert('Integer#/', '15.2.8.3.4') do
  a = 2/1
  b = 2/1.0

  a == 2 and b == 2.0
end

assert('Integer#%', '15.2.8.3.5') do
  a = 1%1
  b = 1%1.0
  c = 2%4

  a == 0 and b == 0.0 and c == 2
end

assert('Integer#<=>', '15.2.8.3.6') do
  a = 1<=>0
  b = 1<=>1
  c = 1<=>2

  a == 1 and b == 0 and c == -1
end

assert('Integer#==', '15.2.8.3.7') do
  a = 1==0
  b = 1==1

  a == false and b == true
end

assert('Integer#~', '15.2.8.3.8') do
  # Complement
  ~0 == -1 and ~2 == -3
end

assert('Integer#&', '15.2.8.3.9') do
  # Bitwise AND
  #   0101 (5)
  # & 0011 (3)
  # = 0001 (1)
  5 & 3 == 1
end

assert('Integer#|', '15.2.8.3.10') do
  # Bitwise OR
  #   0101 (5)
  # | 0011 (3)
  # = 0111 (7)
  5 | 3 == 7
end

assert('Integer#^', '15.2.8.3.11') do
  # Bitwise XOR
  #   0101 (5)
  # ^ 0011 (3)
  # = 0110 (6)
  5 ^ 3 == 6
end

assert('Integer#<<', '15.2.8.3.12') do
  # Left Shift by one
  #   00010111 (23)
  # = 00101110 (46)
  23 << 1 == 46
end

assert('Integer#>>', '15.2.8.3.13') do
  # Right Shift by one
  #   00101110 (46)
  # = 00010111 (23)
  46 >> 1 == 23
end

assert('Integer#ceil', '15.2.8.3.14') do
  10.ceil == 10
end

assert('Integer#downto', '15.2.8.3.15') do
  a = 0
  3.downto(1) do |i|
    a += i
  end
  a == 6
end

assert('Integer#eql?', '15.2.8.3.16') do
  a = 1.eql?(1)
  b = 1.eql?(2)
  c = 1.eql?(nil)

  a == true and b == false and c == false
end

assert('Integer#floor', '15.2.8.3.17') do
  a = 1.floor

  a == 1
end

assert('Integer#next', '15.2.8.3.19') do
  1.next == 2
end

assert('Integer#round', '15.2.8.3.20') do
  1.round == 1
end

assert('Integer#succ', '15.2.8.3.21') do
  1.succ == 2
end

assert('Integer#times', '15.2.8.3.22') do
  a = 0
  3.times do
    a += 1
  end
  a == 3
end

assert('Integer#to_f', '15.2.8.3.23') do
  1.to_f == 1.0
end

assert('Integer#to_i', '15.2.8.3.24') do
  1.to_i == 1
end

assert('Integer#to_s', '15.2.8.3.25') do
  1.to_s == '1' and -1.to_s == "-1"
end

assert('Integer#truncate', '15.2.8.3.26') do
  1.truncate == 1
end

assert('Integer#upto', '15.2.8.3.27') do
  a = 0
  1.upto(3) do |i|
    a += i
  end
  a == 6
end

# Not ISO specified

assert('Integer#step') do
  a = []
  b = []
  1.step(3) do |i|
    a << i
  end
  1.step(6, 2) do |i|
    b << i
  end

  a == [1, 2, 3] and
    b == [1, 3, 5]
end
