# Copyright (c) 2011, David Turnbull <dturnbull at gmail dot com>
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#   * Redistributions of source code must retain the above copyright notice,
#     this list of conditions and the following disclaimer.
#   * Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.


class Redis
  
  #WARN Time complexity may not match C version in this module yet.
  #TODO Should add and delete manipulate @keys instead of clearing?
  
  class ZSet
    include Enumerable

    def initialize
      @hash = Hash.new
	    @keys = nil
	    @keys_reverse = nil
	  end
	  
	  def add(o, s = 0.0)
      @hash[o] = s
	    @keys = nil
	    @keys_reverse = nil
      self
    end

	  def delete(o)
	    @keys = nil
	    @keys_reverse = nil
	    @hash.delete(o)
	    self
	  end
	  
	  def include?(o)
      @hash.include?(o)
    end
	  
	  def score(o)
	    @hash[o]
    end
    
    def size
      @hash.size
    end
    
    def empty?
      @hash.empty?
    end
    
	  def each
	    block_given? or return enum_for(__method__)
	    to_a.each { |o| yield(o) }
	    self
	  end

	  def to_a
	    unless @keys
  	    (@keys = @hash.to_a).sort! do |a, b|
  	      a.reverse <=> b.reverse
	      end
	    end
	    @keys
	  end
	  
	  def to_a_reverse
	    unless @keys_reverse
	      @keys_reverse = to_a.reverse
      end
	    @keys_reverse
    end

    def range reverse, start ,stop, withscores = false
      array = reverse ? to_a_reverse : to_a
      start = start.to_redis_i
      stop = stop.to_redis_i
      start = 0 if start < -size
      return array[start..stop].flatten(1) if withscores
      (array[start..stop]||[]).collect{|i|i.first}
    end
    
    def range_by_score reverse, min, max, *args
      withscores = offset = count = nil
      until args.empty?
        case args.shift.upcase
        when 'LIMIT'
          offset = args.shift.to_i
          count = args.shift.to_i
        when 'WITHSCORES'
          withscores = true
        else
          raise 'bad arguments'
        end
      end
      result = []
      min_exclusive = false
      if min[0] == '('
        min_exclusive = true
        min = min[1..-1]
      end
      min = min.to_redis_f
      max_exclusive = false
      if max[0] == '('
        max_exclusive = true
        max = max[1..-1]
      end
      max = max.to_redis_f
      if reverse
        x = min; min = max; max = x
      end
      (reverse ? to_a_reverse : to_a).each do |member, score|
        next if min > score or (min_exclusive and min >= score)
        next if max < score or (max_exclusive and max <= score)
        if offset
          offset -= 1
          next unless offset < 0
          offset = nil
        end
        result << member
        result << score if withscores
        if count
          count -= 1
          break if count == 0
        end
      end
      result
    end
    
    def self.aggregate database, is_and, destination, numkeys, *args
      numkeys = numkeys.to_i
      aggregate = 'SUM'
      keys = []
      keys << args.shift while (numkeys -= 1) >= 0
      weights = Array.new keys.size, 1
      until args.empty?
        case args.shift.upcase
        when 'WEIGHTS'
          weights = []
          keys.size.times {weights << args.shift.to_redis_f}
        when 'AGGREGATE'
          aggregate = args.shift.upcase
        else
          raise 'bad arguments'
        end
      end
      results = []
      keys.zip(weights) do |key, weight|
        inner_result = ZSet.new
        record = database[key] || ZSet.new
        record.each do |member, score|
          inner_result.add member, (score||1) * weight
        end
        results << inner_result
      end
      result = results.reduce do |memo, result|
        n = is_and ? new : memo
        result.each do |member, score|
          next if is_and and !memo.include?(member)
          test = [score, memo.score(member)].compact
          text << 0 if test.empty?
          case aggregate
          when 'SUM'
            score = test.reduce :+
          when 'MIN'
            score = test.min
          when 'MAX'
            score = test.max
          else
            raise 'bad arguments'
          end
          n.add member, score 
        end
        n
      end
      database[destination] = result unless result.empty?
      result.size
    end
    
  end
  
  module ZSets

    def redis_ZADD key, score, member
      record = (@database[key] ||= ZSet.new)
      result = !record.include?(member)
      record.add member, score.to_redis_f
      result
    end
    
    def redis_ZINCRBY key, increment, member
      record = (@database[key] ||= ZSet.new)
      increment = increment.to_redis_f
      if record.include?(member)
        score = record.score(member) + increment
      else
        score = increment
      end
      raise 'NaN' if score.nan?
      record.add member, score
      score
    end
    
    def redis_ZRANK key, member
      record = (@database[key] || ZSet.new).to_a
      record.index {|i| i[0]==member}
    end
    
    def redis_ZREM key, member
      record = @database[key] || []
      return false unless record.include? member
      record.delete member
      @database.delete key if record.empty?
      return true
    end
    
    def redis_ZSCORE key, member
      (@database[key] || ZSet.new).score member
    end
    
    def redis_ZREVRANK key, member
      record = (@database[key] || ZSet.new).to_a_reverse
      record.index {|i| i[0]==member}
    end
    
    def redis_ZCARD key
      (@database[key] || []).size
    end
    
    def redis_ZREMRANGEBYSCORE key, min, max
      record = @database[key] || ZSet.new
      range = record.range_by_score(false, min, max)
      range.each do |member, score|
        record.delete member
      end
      range.size
    end
    
    def redis_ZCOUNT key, min, max
      record = @database[key] || ZSet.new
      record.range_by_score(false, min, max).size
    end

    def redis_ZREVRANGEBYSCORE key, min, max, *args
      record = @database[key] || ZSet.new
      record.range_by_score true, min, max, *args
    end
    
    def redis_ZRANGEBYSCORE key, min, max, *args
      record = @database[key] || ZSet.new
      record.range_by_score false, min, max, *args
    end
  
    def redis_ZRANGE key, start ,stop, withscores = false
      record = @database[key] || ZSet.new
      record.range false, start ,stop, withscores
    end
  
    def redis_ZREVRANGE key, start ,stop, withscores = false
      record = @database[key] || ZSet.new
      record.range true, start ,stop, withscores
    end

    def redis_ZREMRANGEBYRANK key, start, stop
      record = @database[key] || ZSet.new
      range = record.range false, start ,stop
      range.each do |member, score|
        record.delete member
      end
      range.size
    end
    
    def redis_ZUNIONSTORE destination, numkeys, *args
      ZSet.aggregate @database, false, destination, numkeys, *args      
    end

    def redis_ZINTERSTORE destination, numkeys, *args
      ZSet.aggregate @database, true, destination, numkeys, *args      
    end
    
  end
end
