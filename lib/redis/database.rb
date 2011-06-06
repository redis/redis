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
  class Database
    
    # Redis databases are volatile dictionaries.
    
    attr_reader :blocked_pops, :watchers
    
    def initialize
      @dict = {}
      @expiry = {}
      @blocked_pops = {}
      @watchers = {}
    end
    
    #TOSO touch
    def touch key
      (@watchers[key]||[]).each do |watcher|
        watcher.succeed self, key
      end
    end
    
    def expire key, seconds
      return false unless @dict.has_key? key
      touch key
      @expiry[key] = Time.now + seconds
      return true
    end

    def expire_at key, unixtime
      return false unless @dict.has_key? key
      touch key
      @expiry[key] = Time.at unixtime
      return true
    end
    
    def ttl key
      check_expiry key
      time = @expiry[key]
      return -1 unless time
      (time - Time.now).round
    end
    
    def persist key
      result = @expiry.has_key? key
      touch key if result
      @expiry.delete key
      result
    end
    
    def random_key
      @dict.keys[rand @dict.size]
    end
    
    def [] key
      check_expiry key
      @dict[key]
    end

    def []= key, value
      touch key
      @expiry.delete key
      @dict[key] = value
    end
    
    def has_key? key
      check_expiry key
      @dict.has_key? key
    end
    
    def delete key
      touch key
      @dict.delete key
      @expiry.delete key
    end
    
    def reduce *args, &block
      @dict.reduce *args, &block
    end
    
    def size
      @dict.size
    end
    
    def clear
      # We don't trigger watchers of unset records
      @dict.each_key { |key| touch key }
      @dict.clear
      @expiry.clear
    end
    
    def empty?
      @dict.empty?
    end
    
    private
    
    def check_expiry key
      expires_at = @expiry[key]
      delete key if expires_at and Time.now > expires_at
    end
    
  end
end
