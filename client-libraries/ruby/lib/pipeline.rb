require "redis"

class Redis
  class Pipeline < Redis
    BUFFER_SIZE = 50_000
    
    def initialize(redis)
      @redis = redis
      @commands = []
    end
    
    def get_response
    end
    
    def write(data)
      @commands << data
      write_and_read if @commands.size >= BUFFER_SIZE
    end
    
    def finish
      write_and_read
    end
    
    def write_and_read
      @redis.write @commands.join
      @redis.read_socket
      @commands.clear
    end
    
  end
end