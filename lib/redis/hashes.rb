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
  module Hashes
    
    def redis_HSET key, field, value
      hash = @database[key] ||= {}
      result = !hash.has_key?(field)
      hash[field] = value
      result
    end
    
    def redis_HEXISTS key, field
      (@database[key] || {}).has_key? field
    end

    def redis_HSETNX key, field, value
      hash = @database[key] || {}
      return false if hash.has_key? field
      redis_HSET key, field, value
      return true
    end
    
    def redis_HKEYS key
      (@database[key] || {}).keys
    end
    
    def redis_HVALS key
      (@database[key] || {}).values
    end
    
    def redis_HMSET key, *args
      (@database[key] ||= {}).merge! Hash[*args]
      Response::OK
    end
      
    def redis_HMGET key, *fields
      hash = (@database[key] || {})
      raise 'wrong type' unless Hash === hash
      fields.collect do |field|
        hash[field]
      end
    end
      
    def redis_HLEN key
      (@database[key] || {}).size
    end

    def redis_HGET key, field
      (@database[key] || {})[field]
    end
    
    def redis_HGETALL key
      @database[key] || {}
    end

    def redis_HDEL key, field
      hash = @database[key] || {}
      result = hash.has_key? field
      hash.delete field
      result
    end
    
    def redis_HINCRBY key, field, increment
      hash = @database[key] ||= {}
      value = (hash[field] ||= 0).to_redis_i
      value += increment.to_redis_i
      hash[field] = value
    end
    
  end
end
