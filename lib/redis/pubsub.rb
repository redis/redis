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
  
  module PubSub
    
    class Subscription
      include EventMachine::Deferrable
      
      @@channels ||= {}
      @@pchannels ||= {}
      
      def self.publish channel, message
        subs = @@channels[channel] || []
        subs.each do |sub|
          sub.call channel, message
        end
        sent = subs.size
        (@@pchannels || []).each do |pchannel, subs|
          next unless File.fnmatch(pchannel, channel)
          subs.each do |sub|
            sub.pcall pchannel, channel, message
          end
          sent += subs.size
        end
        sent
      end
      
      def initialize connection
        @connection = connection
        @subs = []
        @psubs = []
      end
      
      def bound
        size > 0
      end
      
      def size
        @subs.size + @psubs.size
      end
      
      def unbind
        unbind_channels
        unbind_patterns
      end
      
      def unbind_channels
        @subs.each do |channel|
          (@@channels[channel] || []).delete self
        end
        @subs.clear
      end
      
      def unbind_patterns
        @psubs.each do |channel|
          (@@pchannels[channel] || []).delete self
        end
        @psubs.clear
      end
      
      def subscribe channel
        c = (@@channels[channel] ||= [])
        unless c.include? self
          c << self 
          @subs << channel
        end
        @connection.send_redis ['subscribe', channel, size]
      end
      
      def psubscribe channel
        c = (@@pchannels[channel] ||= [])
        unless c.include? self
          c << self 
          @psubs << channel
        end
        
        @connection.send_redis ['psubscribe', channel, size]
      end
      
      def unsubscribe channel
        c = (@@channels[channel] || [])
        if c.include? self
          c.delete self
          @subs.delete channel
        end
        @connection.send_redis ['unsubscribe', channel, size]
      end
      
      def punsubscribe channel
        c = (@@pchannels[channel] || [])
        if c.include? self
          c.delete self
          @psubs.delete channel
        end
        @connection.send_redis ['punsubscribe', channel, size]
      end
      
      def call channel, message
        @connection.send_redis ['message', channel, message]
      end

      def pcall pchannel, channel, message
        @connection.send_redis ['pmessage', pchannel, channel, message]
      end
      
    end
    
    def redis_SUBSCRIBE *channels
      sub = @deferred
      sub = Subscription.new self unless Subscription === sub
      channels.each do |channel|
        sub.subscribe channel
      end
      sub
    end
    
    def redis_UNSUBSCRIBE *channels
      sub = @deferred
      sub = Subscription.new self unless Subscription === sub
      if channels.empty?
        sub.unbind_channels
      else
        channels.each do |channel|
          sub.unsubscribe channel
        end
      end
      sub
    end
    
    def redis_PSUBSCRIBE *channels
      sub = @deferred
      sub = Subscription.new self unless Subscription === sub
      channels.each do |channel|
        sub.psubscribe channel
      end
      sub
    end
    
    def redis_PUNSUBSCRIBE *channels
      sub = @deferred
      sub = Subscription.new self unless Subscription === sub
      if channels.empty?
        sub.unbind_patterns
      else
        channels.each do |channel|
          sub.punsubscribe channel
        end
      end
      sub
    end
    
    def redis_PUBLISH channel, message
      Subscription.publish channel, message
    end
    
      
  end
end
