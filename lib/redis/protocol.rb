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
require_relative 'buftok'

class Redis
  
  # Use to respond with raw protocol
  #  Response["+#{data}\n\r"]
  #  Response::OK
  class Response < Array
    OK = self["+OK\r\n"]
    PONG = self["+PONG\r\n"]
    NIL = self["$-1\r\n"]
    NIL_MB = self["*-1\r\n"]
    FALSE = self[":0\r\n"]
    TRUE = self[":1\r\n"]
    QUEUED = self["+QUEUED\r\n"]
  end

  class Watcher
    include EventMachine::Deferrable
    
    attr_reader :bound
    
    def initialize
      @watched = []
      @bound = true
      errback { unbind }
      callback { unbind }
    end
    
    def bind database, *keys
      return unless @bound
      keys.each do |key|
        entry = [database, key]
        next if @watched.include? entry
        @watched << entry
        (database.watchers[key] ||= []).push self
      end
    end
    
    def unbind
      return unless @bound
      @watched.each do |database, key|
        key_df_list = database.watchers[key]
        next unless key_df_list
        key_df_list.delete_if { |e| e == self }
      end
      @bound = false
    end
    
  end
  
  module Protocol

    # Typically raised by redis_QUIT
    class CloseConnection < Exception
    end
  
    def initialize *args
      @buftok = BufferedTokenizer.new
      @multi = nil
      @deferred = nil
      @watcher = nil
      super
    end
    
    def unbind
      @deferred.unbind if @deferred
      @watcher.unbind if @watcher
    end
    
    # Companion to send_data.
    def send_redis data
      if EventMachine::Deferrable === data
        @deferred.unbind if @deferred and @deferred != data
        @deferred = data
      elsif nil == data
        send_data Response::NIL[0]
      elsif false == data
        send_data Response::FALSE[0]
      elsif true == data
        send_data Response::TRUE[0]
      elsif Numeric === data
        data = data.to_f
        if data.nan?
          send_data ":0\r\n"
        elsif !data.infinite?
          int_data = data.to_i
          data = int_data if data == int_data
          send_data ":#{data}\r\n"
        elsif data.infinite? > 0
          send_data ":inf\r\n"
        elsif data.infinite? < 0
          send_data ":-inf\r\n"
        end
      elsif String === data
        send_data "$#{data.size}\r\n"
        send_data data
        send_data "\r\n"
      elsif Response === data
        data.each do |item|
          send_data item
        end
      elsif Hash === data
        send_data "*#{data.size * 2}\r\n"
        data.each do |key, value|
          send_redis key
          send_redis value
        end
      elsif Array === data or Set === data
        send_data "*#{data.size}\r\n"
        data.each do |item|
          if Numeric === item
            int_item = item.to_i
            item = int_item if item == int_item
            send_data ":#{item}\r\n"
          elsif String === item
            send_data "$#{item.size}\r\n"
            send_data item
            send_data "\r\n"
          else
            send_data Response::NIL[0]
          end
        end
      else
        raise "#{data.class} is not a redis type"
      end
    end
    
    def redis_WATCH *keys
      @watcher ||= Watcher.new
      @watcher.bind @database, *keys
      Response::OK
    end
    
    def redis_UNWATCH
      if @watcher
        @watcher.unbind
        @watcher = nil
      end
      Response::OK
    end

    def redis_MULTI
      raise 'MULTI nesting not allowed' if @multi
      @multi = []
      Response::OK
    end
    
    def redis_DISCARD
      redis_UNWATCH
      @multi = nil
      Response::OK
    end

    def redis_EXEC
      if @watcher
        still_bound = @watcher.bound
        redis_UNWATCH
        unless still_bound
          @multi = nil
          return Response::NIL_MB 
        end
      end
      send_data "*#{@multi.size}\r\n"
      response = []
      @multi.each do |strings| 
        result = call_redis *strings
        if EventMachine::Deferrable === result
          result.unbind
          send_redis nil
        else
          send_redis result
        end
      end
      @multi = nil
      Response[]
    end
    
    def call_redis command, *arguments
      send "redis_#{command.upcase}", *arguments
    rescue Exception => e
      raise e if CloseConnection === e
      # Redis.logger.warn "#{command.dump}: #{e.class}:/#{e.backtrace[0]} #{e.message}"
      # e.backtrace[1..-1].each {|bt|Redis.logger.warn bt}
      Response["-ERR #{e.message}\r\n"]
    end
  
    # Process incoming redis protocol
    def receive_data data
      @buftok.extract(data) do |*strings|
        # Redis.logger.warn "#{strings.collect{|a|a.dump}.join ' '}"
        if @multi and !%w{MULTI EXEC DEBUG DISCARD}.include?(strings[0].upcase)
          @multi << strings
          send_redis Response::QUEUED
        else
          send_redis call_redis *strings
        end
      end
    rescue Exception => e
      @buftok.flush
      if CloseConnection === e
        close_connection_after_writing
      else
        send_data "-ERR #{e.message}\r\n" 
      end
    end

  end
end
