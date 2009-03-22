require 'benchmark'
$:.push File.join(File.dirname(__FILE__), 'lib')
require 'redis'

times = 20000

@r = Redis.new
(0..1000000).each{|x|
    @r[x] = "Hello World"
    puts x if (x > 0 and x % 10000) == 0
}
