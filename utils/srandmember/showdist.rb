require 'redis'

r = Redis.new
r.select(9)
r.del("myset");
r.sadd("myset",(0..999).to_a)
freq = {}
100.times {
    res = r.pipelined {
        1000.times {
            r.srandmember("myset")
        }
    }
    res.each{|ele|
        freq[ele] = 0 if freq[ele] == nil
        freq[ele] += 1
    }
}

# Convert into frequency distribution
dist = {}
freq.each{|item,count|
    dist[count] = 0 if dist[count] == nil
    dist[count] += 1
}

min = dist.keys.min
max = dist.keys.max
(min..max).each{|x|
    count = dist[x]
    count = 0 if count == nil
    puts "#{x} -> #{"*"*count}"
}
