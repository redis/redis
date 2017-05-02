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
  
  module Server
    
    def redis_FLUSHDB
      @database.clear
      Response::OK
    end

    def redis_FLUSHALL
      Redis.databases.each do |database|
        database.clear
      end
      Response::OK
    end
    
    def redis_DBSIZE
      @database.size
    end
    
    def redis_DEBUG type, key=nil
      if type.upcase == 'OBJECT'
        "#{@database[key].class}"
        value = @database[key]
        # encoding values are meaningless, they make tcl tests pass
        # and don't forget they need a trailing space
        if String === value
          "Value #{value.class}:#{value.object_id} encoding:raw encoding:int "
        elsif Numeric === value
          "Value #{value.class}:#{value.object_id} encoding:int "
        elsif Array === value
          "Value #{value.class}:#{value.object_id} encoding:ziplist encoding:linkedlist "
        elsif Hash === value
          "Value #{value.class}:#{value.object_id} encoding:zipmap encoding:hashtable "
        elsif Set === value
          "Value #{value.class}:#{value.object_id} encoding:intset encoding:hashtable "
        else
          "Value #{value.class}:#{value.object_id}"
        end
      elsif type.upcase == 'RELOAD'
        "TODO: what is reload"
      else
        raise 'not supported'
      end
    end
    
    def redis_INFO
      [
        "redis_version:%s\r\n",
        "redis_git_sha1:%s\r\n",
        "redis_git_dirty:%d\r\n",
      ].join % [
        Redis::VERSION,
        "Ruby #{RUBY_VERSION}",
        1,
      ]
    end
    
  end
end
