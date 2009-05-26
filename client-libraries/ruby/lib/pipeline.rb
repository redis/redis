require "redis"

class Redis
  class Pipeline < Redis
    BUFFER_SIZE = 50_000
    
    def initialize(redis)
      @redis = redis
      @commands = []
    end
    
    def execute_command(data)
      @commands << data
      write_and_read if @commands.size >= BUFFER_SIZE
    end
    
    def finish
      write_and_read
    end
    
    def write_and_read
      @redis.execute_command(@commands.join, true)
      @redis.read_socket
      @commands.clear
    end
    
  end
end
