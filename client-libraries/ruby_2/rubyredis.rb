# RubyRedis is an alternative implementatin of Ruby client library written
# by Salvatore Sanfilippo.
#
# The aim of this library is to create an alternative client library that is
# much simpler and does not implement every command explicitly but uses
# method_missing instead.

require 'socket'

class RedisClient
    BulkCommands = {
        "set"=>true, "setnx"=>true, "rpush"=>true, "lpush"=>true, "lset"=>true,
        "lrem"=>true, "sadd"=>true, "srem"=>true, "sismember"=>true,
        "echo"=>true, "getset"=>true, "smove"=>true
    }

    def initialize(opts={})
        opts = {:host => 'localhost', :port => '6379', :db => 0}.merge(opts)
        @host = opts[:host]
        @port = opts[:port]
        @db = opts[:db]
        @sock = connect_to_server
        call_command(["select",@db]) if @db != 0
    end

    def to_s
        "Redis Client connected to #{@host}:#{@port} against DB #{@db}"
    end

    def connect_to_server
        TCPSocket.new(@host, @port, 0)
    end

    def method_missing(*argv)
        call_command(argv)
    end

    def call_command(argv)
        bulk = nil
        argv[0] = argv[0].to_s.downcase
        if BulkCommands[argv[0]]
            bulk = argv[-1]
            argv[-1] = bulk.length
        end
        @sock.write(argv.join(" ")+"\r\n")
        @sock.write(bulk+"\r\n") if bulk
        read_reply
    end

    def select(*args)
        raise "SELECT not allowed, use the :db option when creating the object"
    end

    def read_reply
        line = @sock.gets
        case line[0..0]
        when "-"
            raise line.strip
        when "+"
            line[1..-1].strip
        when ":"
            line[1..-1].to_i
        when "$"
            bulklen = line[1..-1].to_i
            return nil if bulklen == -1
            data = @sock.read(bulklen)
            @sock.read(2) # CRLF
            data
        when "*"
            objects = line[1..-1].to_i
            return nil if bulklen == -1
            res = []
            objects.times {
                res << read_reply
            }
            res
        end
    end
end

r = RedisClient.new(:db=>0)
p r.keys("*")
