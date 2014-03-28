# hll-err.rb - Copyright (C) 2014 Salvatore Sanfilippo
# BSD license, See the COPYING file for more information.
#
# Check error of HyperLogLog Redis implementation for different set sizes.

require 'rubygems'
require 'redis'
require 'digest/sha1'

r = Redis.new
r.del('hll')
(1..1000000000).each{|i|
    ele = Digest::SHA1.hexdigest(i.to_s)
    r.hlladd('hll',ele)
    if i != 0 && (i%10000) == 0
        approx = r.hllcount('hll')
        abs_err = (approx-i).abs
        rel_err = 100.to_f*abs_err/i
        puts "#{i} vs #{approx}: #{rel_err}%"
    end
}
