require 'benchmark'
require "#{File.dirname(__FILE__)}/lib/redis"

r = Redis.new
n = (ARGV.shift || 20000).to_i

elapsed = Benchmark.realtime do
  # n sets, n gets
  n.times do |i|
    key = "foo#{i}"
    r[key] = key * 10
    r[key]
  end
end

puts '%.2f Kops' % (2 * n / 1000 / elapsed)
