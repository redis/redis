#--
# = timeout.rb
#
# execution timeout
#
# = Copyright
#
# Copyright - (C) 2008  Evan Phoenix
# Copyright:: (C) 2000  Network Applied Communication Laboratory, Inc.
# Copyright:: (C) 2000  Information-technology Promotion Agency, Japan
#
#++
#
# = Description
#
# A way of performing a potentially long-running operation in a thread, and
# terminating it's execution if it hasn't finished within fixed amount of
# time.
#
# Previous versions of timeout didn't use a module for namespace. This version
# provides both Timeout.timeout, and a backwards-compatible #timeout.
#
# = Synopsis
#
#   require 'timeout'
#   status = Timeout::timeout(5) {
#     # Something that should be interrupted if it takes too much time...
#   }
#

require 'thread'

module Timeout

  ##
  # Raised by Timeout#timeout when the block times out.

  class Error<Interrupt
  end

  # A mutex to protect @requests
  @mutex = Mutex.new

  # All the outstanding TimeoutRequests
  @requests = []

  # Represents +thr+ asking for it to be timeout at in +secs+
  # seconds. At timeout, raise +exc+.
  class TimeoutRequest
    def initialize(secs, thr, exc)
      @left = secs
      @thread = thr
      @exception = exc
    end

    attr_reader :thread, :left

    # Called because +time+ seconds have gone by. Returns
    # true if the request has no more time left to run.
    def elapsed(time)
      @left -= time
      @left <= 0
    end

    # Raise @exception if @thread.
    def cancel
      if @thread and @thread.alive?
        @thread.raise @exception, "execution expired"
      end

      @left = 0
    end

    # Abort this request, ie, we don't care about tracking
    # the thread anymore.
    def abort
      @thread = nil
      @left = 0
    end
  end

  def self.add_timeout(time, exc)

    @controller ||= Thread.new do
      while true
        if @requests.empty?
          sleep
          next
        end

        min = nil

        @mutex.synchronize do
          min = @requests.min { |a,b| a.left <=> b.left }
        end

        slept_for = sleep(min.left)

        @mutex.synchronize do
          @requests.delete_if do |r|
            if r.elapsed(slept_for)
              r.cancel
              true
            else
              false
            end
          end
        end

      end
    end

    req = TimeoutRequest.new(time, Thread.current, exc)

    @mutex.synchronize do
      @requests << req
    end

    @controller.run

    return req
  end

  ##
  # Executes the method's block. If the block execution terminates before +sec+
  # seconds has passed, it returns true. If not, it terminates the execution
  # and raises +exception+ (which defaults to Timeout::Error).
  #
  # Note that this is both a method of module Timeout, so you can 'include
  # Timeout' into your classes so they have a #timeout method, as well as a
  # module method, so you can call it directly as Timeout.timeout().
  
  def timeout(sec, exception=Error)
    return yield if sec == nil or sec.zero?
    raise ThreadError, "timeout within critical session" if Thread.critical

    req = Timeout.add_timeout sec, exception
    
    begin
      yield sec
    ensure
      req.abort
    end
  end

  module_function :timeout

end

##
# Identical to:
#
#   Timeout::timeout(n, e, &block).
#
# Defined for backwards compatibility with earlier versions of timeout.rb, see
# Timeout#timeout.

def timeout(n, e=Timeout::Error, &block) # :nodoc:
  Timeout::timeout(n, e, &block)
end

##
# Another name for Timeout::Error, defined for backwards compatibility with
# earlier versions of timeout.rb.

class Object
  remove_const(:TimeoutError) if const_defined?(:TimeoutError)
end
TimeoutError = Timeout::Error # :nodoc:

if __FILE__ == $0
  p timeout(5) {
    45
  }
  p timeout(5, TimeoutError) {
    45
  }
  p timeout(nil) {
    54
  }
  p timeout(0) {
    54
  }
  p timeout(5) {
    loop {
      p 10
      sleep 1
    }
  }
end

