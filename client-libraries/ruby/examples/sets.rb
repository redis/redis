require 'rubygems'
require 'redis'

r = Redis.new

r.delete 'foo-tags'
r.delete 'bar-tags'

puts
p "create a set of tags on foo-tags"

r.set_add 'foo-tags', 'one'
r.set_add 'foo-tags', 'two'
r.set_add 'foo-tags', 'three'

puts
p "create a set of tags on bar-tags"

r.set_add 'bar-tags', 'three'
r.set_add 'bar-tags', 'four'
r.set_add 'bar-tags', 'five'

puts
p 'foo-tags'

p r.set_members('foo-tags')

puts
p 'bar-tags'

p r.set_members('bar-tags')

puts
p 'intersection of foo-tags and bar-tags'

p r.set_intersect('foo-tags', 'bar-tags')
