# hll-err.rb - Copyright (C) 2014-Present Redis Ltd.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#
# Check error of HyperLogLog Redis implementation for different set sizes.

require 'rubygems'
require 'redis'
require 'digest/sha1'

r = Redis.new
r.del('hll')
i = 0
while true do
    100.times {
        elements = []
        1000.times {
            ele = Digest::SHA1.hexdigest(i.to_s)
            elements << ele
            i += 1
        }
        r.pfadd('hll',elements)
    }
    approx = r.pfcount('hll')
    abs_err = (approx-i).abs
    rel_err = 100.to_f*abs_err/i
    puts "#{i} vs #{approx}: #{rel_err}%"
end
