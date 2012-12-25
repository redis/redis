##
# String ISO Test

assert('String', '15.2.10') do
  String.class == Class
end

assert('String superclass', '15.2.10.2') do
  String.superclass == Object
end

assert('String#*', '15.2.10.5.1') do
  'a' * 5 == 'aaaaa'
end

assert('String#+', '15.2.10.5.2') do
  'a' + 'b' == 'ab'
end

assert('String#<=>', '15.2.10.5.3') do
  a = '' <=> ''
  b = '' <=> 'not empty'
  c = 'not empty' <=> ''
  d = 'abc' <=> 'cba'
  e = 'cba' <=> 'abc'

  a == 0 and b == -1 and c == 1 and
    d == -1 and e == 1
end

assert('String#==', '15.2.10.5.4') do
  'abc' == 'abc' and not 'abc' == 'cba'
end

# TODO: SEGFAULT ATM assert('String#=~', '15.2.10.5.5')

assert('String#[]', '15.2.10.5.6') do
  # length of args is 1
  a = 'abc'[0]
  b = 'abc'[-1]
  c = 'abc'[10]
  d = 'abc'[-10]

  # length of args is 2
  a1 = 'abc'[0, -1]
  b1 = 'abc'[10, 0]
  c1 = 'abc'[-10, 0]
  d1 = 'abc'[0, 0]
  e1 = 'abc'[1, 2]

  # args is RegExp
  # TODO SEGFAULT ATM

  # args is String
  a3 = 'abc'['bc']
  b3 = 'abc'['XX']

  a == 'a' and b == 'c' and c == nil and d == nil and
    a1 == nil and b1 == nil and c1 == nil and d1 == '' and
    e1 == 'bc' and
    a3 == 'bc' and b3 == nil
end

assert('String#[] with Range') do
  a1 = 'abc'[1..0]
  b1 = 'abc'[1..1]
  c1 = 'abc'[1..2]
  d1 = 'abc'[1..3]
  e1 = 'abc'[1..4]
  f1 = 'abc'[0..-2]
  g1 = 'abc'[-2..3]
  h1 = 'abc'[3..4]
  i1 = 'abc'[4..5]
  a2 = 'abc'[1...0]
  b2 = 'abc'[1...1]
  c2 = 'abc'[1...2]
  d2 = 'abc'[1...3]
  e2 = 'abc'[1...4]
  f2 = 'abc'[0...-2]
  g2 = 'abc'[-2...3]
  h2 = 'abc'[3...4]
  i2 = 'abc'[4...5]

  a1 == ''   and b1 == 'b'  and c1 == 'bc' and d1 == 'bc' and e1 == 'bc' and
  f1 == 'ab' and g1 == 'bc' and h1 == ''   and i2 == nil  and
  a2 == ''   and b2 == ''   and c2 == 'b'  and d2 == 'bc' and e2 == 'bc' and
  f2 == 'a'  and g2 == 'bc' and h2 == ''   and i2 == nil
end

assert('String#capitalize', '15.2.10.5.7') do
  a = 'abc'
  a.capitalize

  a == 'abc' and 'abc'.capitalize == 'Abc'
end

assert('String#capitalize!', '15.2.10.5.8') do
  a = 'abc'
  a.capitalize!

  a == 'Abc'
end

assert('String#chomp', '15.2.10.5.9') do
  a = 'abc'.chomp
  b = ''.chomp
  c = "abc\n".chomp
  d = "abc\n\n".chomp
  e = "abc\t".chomp("\t")
  f = "abc\n"

  f.chomp

  a == 'abc' and b == '' and c == 'abc' and
    d == "abc\n" and e == 'abc' and f == "abc\n"
end

assert('String#chomp!', '15.2.10.5.10') do
  a = 'abc'
  b = ''
  c = "abc\n"
  d = "abc\n\n"
  e = "abc\t"

  a.chomp!
  b.chomp!
  c.chomp!
  d.chomp!
  e.chomp!("\t")

  a == 'abc' and b == '' and c == 'abc' and
    d == "abc\n" and e == 'abc'
end

assert('String#chop', '15.2.10.5.11') do
  a = ''.chop
  b = 'abc'.chop
  c = 'abc'

  c.chop

  a == '' and b == 'ab' and c == 'abc'
end

assert('String#chop!', '15.2.10.5.12') do
  a = ''
  b = 'abc'

  a.chop!
  b.chop!

  a == '' and b == 'ab'
end

assert('String#downcase', '15.2.10.5.13') do
  a = 'ABC'.downcase
  b = 'ABC'

  b.downcase

  a == 'abc' and b == 'ABC'
end

assert('String#downcase!', '15.2.10.5.14') do
  a = 'ABC'

  a.downcase!

  a == 'abc'
end

assert('String#each_line', '15.2.10.5.15') do
  a = "first line\nsecond line\nthird line"
  list = ["first line\n", "second line\n", "third line"]
  n_list = []

  a.each_line do |line|
    n_list << line
  end

  list == n_list
end

assert('String#empty?', '15.2.10.5.16') do
  a = ''
  b = 'not empty'

  a.empty? and not b.empty?
end

assert('String#eql?', '15.2.10.5.17') do
  'abc'.eql?('abc') and not 'abc'.eql?('cba')
end

# TODO ATM broken assert('String#gsub', '15.2.10.5.18') do

# TODO ATM broken assert('String#gsub!', '15.2.10.5.19') do

assert('String#hash', '15.2.10.5.20') do
  a = 'abc'

  a.hash == 'abc'.hash
end

assert('String#include?', '15.2.10.5.21') do
  'abc'.include?(97) and not 'abc'.include?(100) and
    'abc'.include?('a') and not 'abc'.include?('d')
end

assert('String#index', '15.2.10.5.22') do
  'abc'.index('a') == 0 and 'abc'.index('d') == nil and
    'abcabc'.index('a', 1) == 3
end

assert('String#initialize', '15.2.10.5.23') do
  a = ''
  a.initialize('abc')

  a == 'abc'
end

assert('String#initialize_copy', '15.2.10.5.24') do
  a = ''
  a.initialize_copy('abc')

  a == 'abc'
end

assert('String#intern', '15.2.10.5.25') do
  'abc'.intern == :abc
end

assert('String#length', '15.2.10.5.26') do
  'abc'.length == 3
end

# TODO Broken ATM assert('String#match', '15.2.10.5.27') do

assert('String#replace', '15.2.10.5.28') do
  a = ''
  a.replace('abc')

  a == 'abc'
end

assert('String#reverse', '15.2.10.5.29') do
  a = 'abc'
  a.reverse

  a == 'abc' and 'abc'.reverse == 'cba'
end

assert('String#reverse!', '15.2.10.5.30') do
  a = 'abc'
  a.reverse!

  a == 'cba' and 'abc'.reverse! == 'cba'
end

assert('String#rindex', '15.2.10.5.31') do
  'abc'.rindex('a') == 0 and 'abc'.rindex('d') == nil and
    'abcabc'.rindex('a', 1) == 0 and 'abcabc'.rindex('a', 4) == 3
end

# TODO Broken ATM assert('String#scan', '15.2.10.5.32') do

assert('String#size', '15.2.10.5.33') do
  'abc'.size == 3
end

assert('String#slice', '15.2.10.5.34') do
  # length of args is 1
  a = 'abc'.slice(0)
  b = 'abc'.slice(-1)
  c = 'abc'.slice(10)
  d = 'abc'.slice(-10)

  # length of args is 2
  a1 = 'abc'.slice(0, -1)
  b1 = 'abc'.slice(10, 0)
  c1 = 'abc'.slice(-10, 0)
  d1 = 'abc'.slice(0, 0)
  e1 = 'abc'.slice(1, 2)

  # slice of shared string
  e11 = e1.slice(0)

  # args is RegExp
  # TODO SEGFAULT ATM

  # args is String
  a3 = 'abc'.slice('bc')
  b3 = 'abc'.slice('XX')

  a == 'a' and b == 'c' and c == nil and d == nil and
    a1 == nil and b1 == nil and c1 == nil and d1 == '' and
    e1 == 'bc' and e11 == 'b' and
    a3 == 'bc' and b3 == nil
end

# TODO Broken ATM
assert('String#split', '15.2.10.5.35') do
  # without RegExp behavior is actually unspecified
  'abc abc abc'.split == ['abc', 'abc', 'abc'] and
    'a,b,c,,d'.split(',') == ["a", "b", "c", "", "d"] and
    'abc abc abc'.split(nil) == ['abc', 'abc', 'abc'] and
    'abc'.split("") == ['a', 'b', 'c']
end

# TODO ATM broken assert('String#sub', '15.2.10.5.36') do

# TODO ATM broken assert('String#sub!', '15.2.10.5.37') do

assert('String#to_i', '15.2.10.5.38') do
  a = ''.to_i
  b = '123456789'.to_i
  c = 'a'.to_i(16)
  d = '100'.to_i(2)

  a == 0 and b == 123456789 and c == 10 and d == 4
end

assert('String#to_f', '15.2.10.5.39') do
  a = ''.to_f
  b = '123456789'.to_f
  c = '12345.6789'.to_f

  check_float(a, 0.0) and check_float(b, 123456789.0) and
    check_float(c, 12345.6789)
end

assert('String#to_s', '15.2.10.5.40') do
  'abc'.to_s == 'abc'
end

assert('String#to_sym', '15.2.10.5.41') do
  'abc'.to_sym == :abc
end

assert('String#upcase', '15.2.10.5.42') do
  a = 'abc'.upcase
  b = 'abc'

  b.upcase

  a == 'ABC' and b == 'abc'
end

assert('String#upcase!', '15.2.10.5.43') do
  a = 'abc'

  a.upcase!

  a == 'ABC'
end

# Not ISO specified

assert('String interpolation (mrb_str_concat for shared strings)') do
  a = "A" * 32
  "#{a}:" == "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA:"
end

assert('Check the usage of a NUL character') do
  "qqq\0ppp"
end

assert('String#bytes') do
  str1 = "hello"
  bytes1 = [104, 101, 108, 108, 111]

  str2 = "\xFF"
  bytes2 = [0xFF]

  str1.bytes == bytes1 and str2.bytes == bytes2
end

assert('String#each_byte') do
  str1 = "hello"
  bytes1 = [104, 101, 108, 108, 111]
  bytes2 = []

  str1.each_byte {|b| bytes2 << b }

  bytes1 == bytes2
end

