require 'rubygems'
require 'ruby-prof'
require "#{File.dirname(__FILE__)}/lib/redis"


mode = ARGV.shift || 'process_time'
n = (ARGV.shift || 200).to_i

r = Redis.new
RubyProf.measure_mode = RubyProf.const_get(mode.upcase)
RubyProf.start

n.times do |i|
  key = "foo#{i}"
  r[key] = key * 10
  r[key]
end

results = RubyProf.stop
File.open("profile.#{mode}", 'w') do |out|
  RubyProf::CallTreePrinter.new(results).print(out)
end
