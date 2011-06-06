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
  
  class Config < Hash
    
    INTEGERS = [:port, :timeout, :databases]
    BOOLEANS = [:daemonize]
    
    def initialize argf
      super()

      # defaults
      self[:dir] = '.'
      self[:logfile] = 'stdout'
      self[:daemonize] = false
      self[:port] = 6379
      self[:pidfile] = "/var/run/redis.pid"
      self[:databases] = 16

      # load from ARGF or IO compatible interface
      argf.each do |line|
        key, val = line.split ' ', 2
        self[key.downcase.gsub(/-/,'_').to_sym] = val.chomp "\n"
      end

      # convert
      INTEGERS.each do |key|
        self[key] = self[key].to_i
      end

      # convert
      BOOLEANS.each do |key|
        next unless String===self[key]
        self[key] = case self[key].downcase
        when 'yes' then true
        when 'no' then false
        else nil
        end
      end
      
    end
    
  end
    
end
