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


require 'set'

class Redis
  module Sets
  
    def redis_SADD key, member
      record = (@database[key] ||= Set.new)
      return false if record.include? member
      record.add member
      true
    end
    
    def redis_SREM key, member
      record = @database[key] || []
      return false unless record.include? member
      record.delete member
      @database.delete key if record.empty?
      return true
    end
    
    # Type checks are only to make tests pass
    def redis_SMOVE source, destination, member
      source_record = @database[source] || Set.new
      dest_record = @database[destination]
      raise 'wrong kind' unless Set === source_record and (!dest_record or Set === dest_record)
      return false unless source_record.include? member
      (@database[destination] ||= Set.new).add member
      source_record.delete member
      @database.delete source if source_record.empty?
      return true
    end
      
    def redis_SCARD key
      (@database[key] || []).size
    end
    
    def redis_SISMEMBER key, member
      (@database[key] || []).include? member
    end
    
    def redis_SMEMBERS key
      (@database[key] || []).to_a
    end
    
    def redis_SINTER *keys
      keys.reduce(nil) { |memo, key| memo ? memo & (@database[key]||[]) : (@database[key]||Set.new) }
    end
    
    def redis_SINTERSTORE destination, *keys
      record = redis_SINTER *keys
      if record.empty?
        @database.delete destination 
      else
        @database[destination] = record
      end
      record.size
    end

    def redis_SUNION *keys
      keys.reduce(Set.new) { |memo, key| memo | (@database[key]||[]) }
    end

    def redis_SUNIONSTORE destination, *keys
      record = redis_SUNION *keys
      if record.empty?
        @database.delete destination 
      else
        @database[destination] = record
      end
      record.size
    end
    
    def redis_SDIFF *keys
      keys.reduce(nil) { |memo, key| memo ? memo - (@database[key]||[]) : (@database[key]||Set.new) }
    end
    
    def redis_SDIFFSTORE destination, *keys
      (@database[destination] = redis_SDIFF *keys).size
    end
    
    def redis_SPOP key
      set = @database[key]
      return nil unless set
      rec = rand set.size
      result = set.to_a[rec]
      set.delete result
      @database.delete key if set.empty?
      result
    end
    
    def redis_SRANDMEMBER key
      set = (@database[key] || [])
      return nil if set.empty?
      set.to_a[rand set.size]
    end
    
  end
end
