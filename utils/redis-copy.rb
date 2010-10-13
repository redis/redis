# redis-copy.rb - Copyright (C) 2009-2010 Salvatore Sanfilippo
# BSD license, See the COPYING file for more information.
#
# Copy the whole dataset from one Redis instance to another one
#
# WARNING: currently hashes and sorted sets are not supported! This
#          program should be updated.

require 'rubygems'
require 'redis'
require 'digest/sha1'

def redisCopy(opts={})
    sha1=""
    src = Redis.new(:host => opts[:srchost], :port => opts[:srcport])
    dst = Redis.new(:host => opts[:dsthost], :port => opts[:dstport])
    puts "Loading key names..."
    keys = src.keys('*')
    puts "Copying #{keys.length} keys..."
    c = 0
    keys.each{|k|
        vtype = src.type?(k)
        ttl = src.ttl(k).to_i if vtype != "none"

        if vtype == "string"
            dst[k] = src[k]
        elsif vtype == "list"
            list = src.lrange(k,0,-1)
            if list.length == 0
                # Empty list special case
                dst.lpush(k,"")
                dst.lpop(k)
            else
                list.each{|ele|
                    dst.rpush(k,ele)
                }
            end
        elsif vtype == "set"
            set = src.smembers(k)
            if set.length == 0
                # Empty set special case
                dst.sadd(k,"")
                dst.srem(k,"")
            else
                set.each{|ele|
                    dst.sadd(k,ele)
                }
            end
        elsif vtype == "none"
            puts "WARNING: key '#{k}' was removed in the meanwhile."
        end

        # Handle keys with an expire time set
        if ttl != -1 and vtype != "none"
            dst.expire(k,ttl)
        end

        c = c+1
        if (c % 1000) == 0
            puts "#{c}/#{keys.length} completed"
        end
    }
    puts "DONE!"
end

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
