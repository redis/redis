require 'rubygems'
require 'redis'

r = Redis.new

r.delete('foo')

puts 

p'set foo to "bar"'
r['foo'] = 'bar'

puts

p 'value of foo'
p r['foo']
