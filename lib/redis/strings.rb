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
  
  # Note: Redis does not have a dedicated integer type.
  # Integer operations will convert values to integers.
  
  module Strings
    
    def redis_GET key
      value = @database[key]
      found = [NilClass, String, Numeric].find { |klass| klass === value }
      raise 'wrong kind' unless found
      value
    end
    
    def redis_APPEND key, value
      new_value = redis_GET(key).to_s + value
      @database[key] = new_value
      new_value.size
    end
    
    def redis_SETRANGE key, offset, value
      data = (redis_GET(key)||'').to_s
      return data.size if value.empty?
      offset = offset.to_redis_pos_i
      raise 'maximum allowed size' if offset + value.size > 512*1024*1024
      if data.size <= offset
        data.concat 0.chr * (offset - data.size + value.size)
      end
      data[offset,value.size] = value
      @database[key] = data
      data.size
    end
    
    def redis_GETRANGE key, first, last
      first = first.to_redis_i
      last = last.to_redis_i
      value = redis_GET(key) || ''
      first = 0 if first < -value.size
      value[first..last]
    end
    
    def redis_GETBIT key, offset
      data = redis_GET key
      return 0 unless data
      data = data.to_s
      offset = offset.to_redis_pos_i
      byte = offset / 8
      bit = 0x80 >> offset % 8
      return 0 if data.size <= byte
      original_byte = data[byte].ord
      original_bit = original_byte & bit
      original_bit != 0
    end
    
    def redis_SETBIT key, offset, value
      data = (redis_GET(key)||'').to_s
      offset = offset.to_redis_pos_i
      raise 'out of range' if offset >= 4*1024*1024*1024
      byte = offset / 8
      bit = 0x80 >> offset % 8
      if data.size <= byte
        data += 0.chr * (byte - data.size + 1)
      end
      original_byte = data[byte].ord
      original_bit = original_byte & bit
      if value == '0'
        data[byte] = (original_byte & ~bit).chr
      elsif value == '1'
        data[byte] = (original_byte | bit).chr
      else
        raise 'out of range'
      end
      @database[key] = data
      original_bit != 0
    end
  
    def redis_STRLEN key
      value = redis_GET key
      value ? value.size : 0
    end

    def redis_MGET *keys
      keys.collect do |key|
        value = @database[key]
        (String === value) ? value : nil
      end
    end
    
    def redis_GETSET key, value
      old = redis_GET key
      @database[key] = value
      old
    end
    
    def redis_SET key, value
      @database[key] = value
      Response::OK
    end

    def redis_SETEX key, seconds, value
      @database[key] = value
      @database.expire key, seconds.to_redis_pos_i
      Response::OK
    end
    
    def redis_MSET *args
      Hash[*args].each do |key, value|
        @database[key] = value
      end
      Response::OK
    end
    
    def redis_MSETNX *args
      hash = Hash[*args]
      hash.each do |key, value|
        return false if @database.has_key? key
      end
      hash.each do |key, value|
        @database[key] = value
      end
      true
    end

    def redis_SETNX key, value
      return false if @database.has_key? key
      @database[key] = value
      true
    end
    
    def redis_INCR key
      @database[key] = (@database[key] || 0).to_redis_i + 1
    end

    def redis_INCRBY key, value
      @database[key] = (@database[key] || 0).to_redis_i + value.to_redis_i
    end

    def redis_DECRBY key, value
      @database[key] = (@database[key] || 0).to_redis_i - value.to_redis_i
    end
      
  end
end
