require 'rubygems'
require 'redis'

r = Redis.new

r.delete 'logs'

puts

p "pushing log messages into a LIST"
r.push_tail 'logs', 'some log message'
r.push_tail 'logs', 'another log message'
r.push_tail 'logs', 'yet another log message'
r.push_tail 'logs', 'also another log message'

puts
p 'contents of logs LIST'

p r.list_range('logs', 0, -1)

puts
p 'Trim logs LIST to last 2 elements(easy circular buffer)'

r.list_trim('logs', -2, -1)

p r.list_range('logs', 0, -1)
