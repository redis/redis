# redis-copy.rb - Copyright (C) 2009-2010 Salvatore Sanfilippo
# BSD license, See the COPYING file for more information.
#
# Copy the whole dataset from one Redis instance to another one
#
# WARNING: this utility is deprecated and serves as a legacy adapter
#          for the more-robust redis-copy gem.

require 'shellwords'

def redisCopy(opts={})
  src = "#{opts[:srchost]}:#{opts[:srcport]}"
  dst = "#{opts[:dsthost]}:#{opts[:dstport]}"
  `redis-copy #{src.shellescape} #{dst.shellescape}`
rescue Errno::ENOENT
  $stderr.puts 'This utility requires the redis-copy executable',
               'from the redis-copy gem on https://rubygems.org',
               'To install it, run `gem install redis-copy`.'
  exit 1
end

$stderr.puts "This utility is deprecated. Use the redis-copy gem instead."
if ARGV.length != 4
    puts "Usage: redis-copy.rb <srchost> <srcport> <dsthost> <dstport>"
    exit 1
end
puts "WARNING: it's up to you to FLUSHDB the destination host before to continue, press any key when ready."
STDIN.gets
srchost = ARGV[0]
srcport = ARGV[1]
dsthost = ARGV[2]
dstport = ARGV[3]
puts "Copying #{srchost}:#{srcport} into #{dsthost}:#{dstport}"
redisCopy(:srchost => srchost, :srcport => srcport.to_i,
          :dsthost => dsthost, :dstport => dstport.to_i)
