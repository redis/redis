# hll-err.rb - Copyright (C) 2014 Salvatore Sanfilippo
# BSD license, See the COPYING file for more information.
#
# This program is suited to output average and maximum errors of
# the Redis HyperLogLog implementation in a format suitable to print
# graphs using gnuplot.

require 'rubygems'
require 'redis'
require 'digest/sha1'

# Generate an array of [cardinality,relative_error] pairs
# in the 0 - max range with step of 1000*step.
#
# 'r' is the Redis object used to perform the queries.
# 'seed' must be different every time you want a test performed
# with a different set. The function guarantees that if 'seed' is the
# same, exactly the same dataset is used, and when it is different,
# a totally unrelated different data set is used (without any common
# element in practice).
def run_experiment(r,seed,max,step)
    r.del('hll')
    i = 0
    samples = []
    while i < max do
        step.times {
            elements = []
            1000.times {
                ele = Digest::SHA1.hexdigest(i.to_s+seed.to_s)
                elements << ele
                i += 1
            }
            r.hlladd('hll',*elements)
        }
        approx = r.hllcount('hll')
        err = approx-i
        rel_err = 100.to_f*err/i
        samples << [i,rel_err]
    end
    samples
end

def filter_samples(numsets,filter)
    r = Redis.new
    dataset = {}
    (0...numsets).each{|i|
        dataset[i] = run_experiment(r,i,100000,1)
    }
    dataset[0].each_with_index{|ele,index|
        if filter == :max
            card=ele[0]
            err=ele[1].abs
            (1...numsets).each{|i|
                err = dataset[i][index][1] if err < dataset[i][index][1]
            }
            puts "#{card} #{err}"
        elsif filter == :avg
            card=ele[0]
            err = 0
            (0...numsets).each{|i|
                err += dataset[i][index][1]
            }
            err /= numsets
            puts "#{card} #{err}"
        elsif filter == :all
            (0...numsets).each{|i|
                card,err = dataset[i][index]
                puts "#{card} #{err}"
            }
        else
            raise "Unknown filter #{filter}"
        end
    }
end

filter_samples(100,:all)
#filter_samples(100,:max)
#filter_samples(100,:avg)
