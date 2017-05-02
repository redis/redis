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


require 'logger'

class Redis
  
  # Create log entry example:
  # Redis.logger.notice "Server started, Redis version %s (Ruby)" % Redis::VERSION
  # Change device example:
  # Redis.logger config[:logfile] unless config[:logfile] == 'stdout'
  def self.logger(logdev = nil, *opts)
    @@logger = nil if logdev
    @@logger ||= lambda {
      logger = Logger.new (logdev||STDOUT), *opts
      logger
    }.call
  end
  
  # Redis levels are: DEBUG < INFO < NOTICE < WARNING
  # This logger inserts support for NOTICE
  class Logger < ::Logger
    
    def initialize(logdev, *args)
      super
      @raw_logdev = logdev
      @default_formatter = proc { |severity, datetime, progname, msg|
        mark = case severity
        when 'DEBUG' then '.'
        when 'INFO' then '-'
        when 'NOTE' then '*'
        when 'WARN' then '#'
        else '!'
        end
        "[#{Process.pid}] #{datetime.strftime '%d %b %H:%H:%S'} #{mark} #{msg}\n"
      }
    end
    
    def flush
      @raw_logdev.flush if @raw_logdev.respond_to? :flush
    end
    
    module Severity
      # logger.rb says "max 5 char" for labels
      SEV_LABEL = %w(DEBUG INFO NOTE WARN ERROR FATAL ANY)
      DEBUG = 0
      INFO = 1
      NOTICE = 2
      WARN = 3
      ERROR = 4
      FATAL = 5
      UNKNOWN = 6
    end
    include Severity

    def notice(progname = nil, &block)
      add(NOTICE, nil, progname, &block)
    end
    def warn(progname = nil, &block)
      add(WARN, nil, progname, &block)
    end
    def error(progname = nil, &block)
      add(ERROR, nil, progname, &block)
    end
    def fatal(progname = nil, &block)
      add(FATAL, nil, progname, &block)
    end
    def unknown(progname = nil, &block)
      add(UNKNOWN, nil, progname, &block)
    end
    
    def notice?; @level <= NOTICE; end
    def warn?; @level <= WARN; end
    def error?; @level <= ERROR; end
    def fatal?; @level <= FATAL; end
        
    private
    
    def format_severity(severity)
      SEV_LABEL[severity] || 'ANY'
    end
    
  end
  
  
end
