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


require_relative '../redis'
require_relative 'config'
require_relative 'protocol'
require_relative 'server'
require_relative 'keys'
require_relative 'strings'
require_relative 'lists'
require_relative 'sets'
require_relative 'zsets'
require_relative 'hashes'
require_relative 'pubsub'

require 'eventmachine'

class Redis
  class Connection < EventMachine::Connection
    
    include Protocol
    
    def initialize options=nil
      @options = options || Config.new
      @database = Redis.databases[0]
      authorize unless options[:requirepass]
      super()
    end
    
    def authorize
      return if @authorized
      extend Server
      extend Keys
      extend Strings
      extend Lists
      extend Sets
      extend ZSets
      extend Hashes
      extend PubSub
      @authorized = true
    end
    
    def redis_AUTH password
      raise 'invalid password' unless password == @options[:requirepass]
      authorize
      Response::OK
    end

    def redis_SELECT db_index
      database = Redis.databases[db_index.to_redis_i]
      raise 'index out of range' unless database
      @database = database
      Response::OK
    end
    
    def redis_PING
      Response::PONG
    end

    def redis_ECHO str
      str
    end

    def redis_QUIT
      send_redis Response::OK
      raise CloseConnection
    end
    
  end
end