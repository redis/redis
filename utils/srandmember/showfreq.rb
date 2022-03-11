require 'redis'

r = Redis.new
r.select(9)
r.del("myset");
r.sadd("myset",(0..999).to_a)
freq = {}
500.times {
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

# Print the frequency each element was yield to process it with gnuplot
freq.each{|item,count|
    puts "#{item} #{count}"
}
