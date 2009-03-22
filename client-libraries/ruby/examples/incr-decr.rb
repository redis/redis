require 'rubygems'
require 'redis'

r = Redis.new

puts
p 'incr'
r.delete 'counter'

p r.incr('counter')
p r.incr('counter')
p r.incr('counter')

puts
p 'decr'
p r.decr('counter')
p r.decr('counter')
p r.decr('counter')
