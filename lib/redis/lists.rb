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


require 'eventmachine'

class Redis
  
  module Lists
    
    class DeferredPop
      include EventMachine::Deferrable
      
      attr_reader :bound
      
      def initialize database, timeout_secs, *keys
        @database = database
        @keys = keys
        timeout timeout_secs if timeout_secs > 0
        errback { unbind }
        callback { unbind }
        keys.each do |key|
          (@database.blocked_pops[key] ||= []).push self
        end
        @bound = true
      end
      
      def unbind
        return unless @bound
        @keys.each do |key|
          key_df_list = @database.blocked_pops[key]
          next unless key_df_list
          key_df_list.delete_if { |e| e == self }
        end
        @bound = false
      end
      
    end
    
    def redis_LRANGE key, first, last
      first = first.to_redis_i
      last = last.to_redis_i
      list = @database[key] || []
      first = 0 if first < -list.size
      list[first..last]
    end
    
    def redis_LTRIM key, start, stop
      @database[key] = redis_LRANGE key, start, stop
    end
    
    def redis_BRPOP *args
      timeout = args.pop.to_redis_pos_i
      args.each do |key|
        list = @database[key]
        if list and list.size > 0
          value = list.pop
          @database.delete key if list.empty?
          return [key, value]
        end
      end
      df = DeferredPop.new(@database, timeout, *args)
      df.errback { send_redis Response::NIL_MB }
      df.callback { |key, value| send_redis [key, value] }
      df
    end
    
    def redis_BLPOP *args
      timeout = args.pop.to_redis_pos_i
      args.each do |key|
        list = @database[key]
        if list and list.size > 0
          value = list.shift
          @database.delete key if list.empty?
          return [key, value]
        end
      end
      df = DeferredPop.new(@database, timeout, *args)
      df.errback { send_redis Response::NIL_MB }
      df.callback { |key, value| send_redis [key, value] }
      df
    end

    #TODO The redis tests require a specific error so we can't
    # let Ruby do the error handling.  Make better tests so we
    # kill the rampant raise 'wrong kind' ...
    
    def redis_RPOPLPUSH source, destination
      source_list = @database[source]
      return nil unless source_list
      raise 'wrong kind' unless Array === source_list
      raise 'wrong kind' unless !@database[destination] or Array === @database[destination]
      value = source_list.pop
      @database.delete source if source_list.empty?
      redis_LPUSH destination, value
      return value
    end
    
    def redis_BRPOPLPUSH source, destination, timeout
      source_list = @database[source]
      if source_list
        raise 'wrong kind' unless Array === source_list
        value = source_list.pop
        @database.delete source if source_list.empty?
        redis_LPUSH destination, value
        return value
      end
      raise 'wrong kind' unless !@database[destination] or Array === @database[destination]
      df = DeferredPop.new @database, timeout.to_redis_pos_i, source
      df.errback {send_redis Response::NIL_MB}
      df.callback do |key, value|
        redis_LPUSH destination, value
        send_redis value
      end
      df
    end
    
    def redis_RPUSH key, value
      list = @database[key]
      raise 'wrong kind' unless !list or Array === list
      (@database.blocked_pops[key] ||= []).each do |deferrable|
        deferrable.succeed key, value
        return 0
      end
      list = @database[key] = [] unless list
      list.push(value).size
    end

    def redis_LPUSH key, value
      list = @database[key]
      raise 'wrong kind' unless !list or Array === list
      (@database.blocked_pops[key] ||= []).each do |deferrable|
        deferrable.succeed key, value
        return 0
      end
      list = @database[key] = [] unless list
      list.unshift(value).size
    end

    def redis_LPUSHX key, value
      list = @database[key]
      return 0 unless Array === list and list.size > 0
      redis_LPUSH key, value
      list.size
    end

    def redis_RPUSHX key, value
      list = @database[key]
      return 0 unless Array === list and list.size > 0
      redis_RPUSH key, value
      list.size
    end
    
    def redis_LINSERT key, mode, pivot, value
      list = @database[key]
      index = list.find_index pivot
      return -1 unless index
      case mode.upcase
      when 'BEFORE'
        list[index,0] = value
      when 'AFTER'
        list[index+1,0] = value
      else
        raise 'only BEFORE|AFTER supported'
      end
      list.size
    end
    
    def redis_RPOP key
      list = @database[key]
      return nil unless list
      value = list.pop
      @database.delete key if list.empty?
      value
    end

    def redis_LPOP key
      list = @database[key]
      return nil unless list
      value = list.shift
      @database.delete key if list.empty?
      value
    end

    def redis_LLEN key
      list = @database[key] || []
      raise 'wrong kind' unless Array === list
      list.size
    end

    def redis_LINDEX key, index
      list = @database[key] || []
      raise 'wrong kind' unless Array === list
      list[index.to_redis_i]
    end

    def redis_LSET key, index, value
      list = @database[key] || []
      raise 'key value wrong kind' unless Array === list
      raise 'out of range' unless list.size > index.to_redis_i.abs
      list[index.to_redis_i] = value
    end
    
    def redis_LREM key, count, value
      list = @database[key] || []
      count = count.to_redis_i
      size = list.size
      if count == 0
        list.delete value
      elsif count < 0
        i = list.size
        while i > 0 and count < 0
          i -= 1
          if list[i] == value
            list.delete_at i
            count += 1
          end
        end
      else # count > 0
        i = 0
        while i < list.size and count > 0
          if list[i] == value
            list.delete_at i 
            count -= 1
          else
            i += 1
          end
        end
      end
      size - list.size
    end
      
  end
end
