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
  module Keys
    
    def redis_RANDOMKEY
      return Response::NIL if @database.empty?
      @database.random_key
    end
  
    def redis_KEYS pattern
      @database.reduce([]) do |memo, key_val|
        key = key_val[0]
        memo.push key if File.fnmatch(pattern, key)
        memo
      end
    end
    
    def redis_SORT key, *args
      record = @database[key] || []
      sort = 'ASC'
      gets = []
      alpha = false
      by = by_hash = offset = count = store = nil
      until args.empty?
        arg = args.shift
        case arg.upcase
        when 'LIMIT'
          offset = args.shift.to_i
          count = args.shift.to_i
        when 'ASC'
          sort = 'ASC'
        when 'DESC'
          sort = 'DESC'
        when 'ALPHA'
          alpha = true
        when 'STORE'
          store = args.shift
        when 'GET'
          gets << args.shift
        when 'BY'
          by, by_hash = args.shift.split '->', 2
        else
          raise "#{arg} bad argument"
        end
      end
      result = record.sort do |a, b|
        if by
          a = @database[by.sub /\*/, a]
          a = a[by_hash] if by_hash
          b = @database[by.sub /\*/, b]
          b = b[by_hash] if by_hash
        end
        if alpha
          a = a.to_s
          b = b.to_s
        else
          a = a.to_f
          b = b.to_f
        end
        if sort == 'DESC'
          b <=> a
        else
          a <=> b
        end
      end
      unless gets.empty?
        original = result
        result = []
        original.each do |r|
          gets.each do |g|
            get, get_hash = g.split('->', 2)
            r = @database[get.sub /\*/, r] unless get == '#'
            r = r[get_hash] if get_hash
            result << r
          end
        end
      end
      if count and offset
        result = result[offset,count]
      elsif count
        result = result[0,count]
      elsif offset
        result = result[offset..-1]
      end
      if Array === result[0]
        result = result.collect {|r| r.first}
      end
      @database[store] = result if store
      result
    end
    
    def redis_DEL *keys
      count = 0
      keys.each do |key|
        count += 1  if @database.has_key? key
        @database.delete key
      end
      count
    end
    
    def redis_TYPE key
      if String === @database[key]
        'string'
      elsif Numeric === @database[key]
        'string'
      elsif Array === @database[key]
        'list'
      elsif Set === @database[key]
        'set'
      elsif ZSet === @database[key]
        'zset'
      elsif Hash === @database[key]
        'hash'
      else
        'unknown'
      end
    end

    def redis_EXISTS key
      @database.has_key? key
    end
    
    def redis_EXPIRE key, seconds
      @database.expire key, seconds.to_redis_pos_i
    end

    def redis_EXPIREAT key, timestamp
      @database.expire_at key, timestamp.to_redis_pos_i
    end
    
    def redis_PERSIST key
      @database.persist key
    end

    def redis_TTL key
      @database.ttl key
    end
    
    def redis_RENAME key, newkey
      raise 'key and newkey are identical' if key == newkey
      raise 'key not found' unless @database.has_key? key
      @database[newkey] = @database[key]
      @database.delete key
    end
    
    def redis_RENAMENX key, newkey
      return false if @database.has_key? newkey
      redis_RENAME key, newkey
      true
    end
    
    def redis_MOVE key, db
      raise unless @database.has_key? key
      raise if Redis.databases[db.to_redis_i].has_key? key
      Redis.databases[db.to_redis_i][key] = @database[key]
      @database.delete key
      true
    rescue
      false
    end
    
      
  end
end
