##
# Range ISO Test

assert('Range', '15.2.14') do
  Range.class == Class
end

assert('Range superclass', '15.2.14.2') do
  Range.superclass == Object
end

assert('Range#==', '15.2.14.4.1') do
  (1..10) == (1..10) and not (1..10) == (1..100)
end

assert('Range#===', '15.2.14.4.2') do
  a = (1..10)

  a === 5 and not a === 20
end

assert('Range#begin', '15.2.14.4.3') do
  (1..10).begin == 1
end

assert('Range#each', '15.2.14.4.4') do
  a = (1..3)
  b = 0
  a.each {|i| b += i}
  b == 6
end

assert('Range#end', '15.2.14.4.5') do
  (1..10).end == 10
end

assert('Range#exclude_end?', '15.2.14.4.6') do
  (1...10).exclude_end? and not (1..10).exclude_end?
end

assert('Range#first', '15.2.14.4.7') do
  (1..10).first == 1
end

assert('Range#include', '15.2.14.4.8') do
  a = (1..10)

  a.include?(5) and not a.include?(20)
end

# TODO SEGFAULT ATM
#assert('Range#initialize', '15.2.14.4.9') do
#  a = Range.new(1, 10, true)
#  b = Range.new(1, 10, false)
#
#  a == (1..10) and a.exclude_end? and b == (1..10) and
#    not b.exclude_end?
#end

assert('Range#last', '15.2.14.4.10') do
  (1..10).last == 10
end

assert('Range#member?', '15.2.14.4.11') do
  a = (1..10)

  a.member?(5) and not a.member?(20)
end
