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
  class BufferedTokenizer < Array
    
    # Minimize the amount of memory copying.
    # Similar to EventMachine::BufferedTokenizer.
    # This will be ported to C when complete.
    
    #TODO max buffer size
    
    def initialize
      super()
      @split = nil
      @pending = nil
      @binary_size = nil
      @remaining = 0
      @elements = []
    end
    
    def extract data  
      unshift_split if @split
      push data
      frame do |str|
        @elements << str
        if @remaining > 0
          @remaining -= 1
          next unless @remaining == 0
        end
        yield *@elements unless @elements.empty?
        @elements.clear
      end
    end
    
    def flush
      @split = nil
      clear
      nil
    end
    
    private
    
    # The primary performance trick is to String#split and work with that.
    def unshift_split
      unshift @split.join "\n"
      @split = nil
    end
        
    # yields redis data until no more found in buffer
    def frame
      while true
        if @binary_size
          s = read @binary_size
          break unless s
          @binary_size = nil
          yield s
        else
          line = gets
          break unless line
          case line[0]
          when '*'
            @remaining = line[1..-1].to_i
            if @remaining > 1024*1024
              @remaining = 0
              raise "Protocol error: invalid multibulk length"
            end
          when '$'
            @binary_size = line[1..-1].to_i
            if @binary_size == -1
              @binary_size = nil
              yield nil
            elsif (@binary_size == 0 and line[1] != '0') or @binary_size < 0 or @binary_size > 512*1024*1024
              @binary_size = nil
              raise "Protocol error: invalid bulk length"
            end
          else
            raise "expected '$', got '#{line[0]}'" if @remaining > 0
            parts = line.split(' ')
            @remaining = parts.size
            parts.each {|l| yield l}
          end
        end
      end
    end
    
    # Read a binary redis token, nil if none available
    def read length
      if @split
        if @split.first.size >= length
          result = @split.shift[0...length]
          unshift_split if @split.size == 1
          return result
        end
        unshift_split
      end
      unless @pending
        size = reduce(0){|x,y|x+y.size}
        return nil unless size >= length
        @pending = dup
        clear
        remainder = size - length
        if remainder > 0
          last_string = @pending[-1]
          @pending[-1] = last_string[0...-remainder]
          push last_string[-remainder..-1]
        end
      end
      # eat newline
      return nil unless gets
      result = @pending.join
      @pending = nil
      result
    end

    # Read a newline terminated redis token, nil if none available
    def gets
      unless @split
        @split = join.split "\n", -1
        clear
      end
      if @split.size > 1
        result = @split.shift.chomp "\n"
      else
        result = nil
      end
      unshift_split if @split.size == 1
      result
    end
    
  end
end
