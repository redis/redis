require 'socket'
require File.join(File.dirname(__FILE__),'pipeline')

begin
    if (RUBY_VERSION >= '1.9')
        require 'timeout'
        RedisTimer = Timeout
    else
        require 'system_timer'
        RedisTimer = SystemTimer
    end
rescue LoadError
    RedisTimer = nil
end

class Redis
    BulkCommands = {
        "set"=>true, "setnx"=>true, "rpush"=>true, "lpush"=>true, "lset"=>true,
        "lrem"=>true, "sadd"=>true, "srem"=>true, "sismember"=>true,
        "echo"=>true, "getset"=>true, "smove"=>true
    }

    ConvertToBool = lambda do |r| 
      case r 
        when 0 then false
        when 1 then true
        else r
      end
    end

    ReplyProcessor = {
        "exists" => ConvertToBool,
        "sismember"=> ConvertToBool,
        "sadd"=> ConvertToBool,
        "srem"=> ConvertToBool,
        "smove"=> ConvertToBool,
        "move"=> ConvertToBool,
        "setnx"=> ConvertToBool,
        "del"=> ConvertToBool,
        "renamenx"=> ConvertToBool,
        "expire"=> ConvertToBool,
        "keys" => lambda{|r| r.split(" ")},
        "info" => lambda{|r| 
            info = {}
            r.each_line {|kv|
                k,v = kv.split(":",2).map{|x| x.chomp}
                info[k.to_sym] = v
            }
            info
        }
    }

    Aliases = {
        "flush_db" => "flushdb",
        "flush_all" => "flushall",
        "last_save" => "lastsave",
        "key?" => "exists",
        "delete" => "del",
        "randkey" => "randomkey",
        "list_length" => "llen",
        "push_tail" => "rpush",
        "push_head" => "lpush",
        "pop_tail" => "rpop",
        "pop_head" => "lpop",
        "list_set" => "lset",
        "list_range" => "lrange",
        "list_trim" => "ltrim",
        "list_index" => "lindex",
        "list_rm" => "lrem",
        "set_add" => "sadd",
        "set_delete" => "srem",
        "set_count" => "scard",
        "set_member?" => "sismember",
        "set_members" => "smembers",
        "set_intersect" => "sinter",
        "set_intersect_store" => "sinterstore",
        "set_inter_store" => "sinterstore",
        "set_union" => "sunion",
        "set_union_store" => "sunionstore",
        "set_diff" => "sdiff",
        "set_diff_store" => "sdiffstore",
        "set_move" => "smove",
        "set_unless_exists" => "setnx",
        "rename_unless_exists" => "renamenx",
        "type?" => "type"
    }

    def initialize(opts={})
        @host = opts[:host] || '127.0.0.1'
        @port = opts[:port] || 6379
        @db = opts[:db] || 0
        @timeout = opts[:timeout] || 5
        $debug = opts[:debug] || false
        connect_to_server
    end

    def to_s
        "Redis Client connected to #{@host}:#{@port} against DB #{@db}"
    end

    def connect_to_server
        @sock = connect_to(@host,@port,@timeout == 0 ? nil : @timeout)
        call_command(["select",@db]) if @db != 0
    end

    def connect_to(host, port, timeout=nil)
        # We support connect() timeout only if system_timer is availabe
        # or if we are running against Ruby >= 1.9
        # Timeout reading from the socket instead will be supported anyway.
        if @timeout != 0 and RedisTimer
            begin
                sock = TCPSocket.new(host, port)
            rescue Timeout::Error
                @sock = nil
                raise Timeout::Error, "Timeout connecting to the server"
            end
        else
            sock = TCPSocket.new(host, port)
        end
        sock.setsockopt Socket::IPPROTO_TCP, Socket::TCP_NODELAY, 1

        # If the timeout is set we set the low level socket options in order
        # to make sure a blocking read will return after the specified number
        # of seconds. This hack is from memcached ruby client.
        if timeout
            secs = Integer(timeout)
            usecs = Integer((timeout - secs) * 1_000_000)
            optval = [secs, usecs].pack("l_2")
            sock.setsockopt Socket::SOL_SOCKET, Socket::SO_RCVTIMEO, optval
            sock.setsockopt Socket::SOL_SOCKET, Socket::SO_SNDTIMEO, optval
        end
        sock
    end

    def method_missing(*argv)
        call_command(argv)
    end

    def call_command(argv)
        puts argv.inspect if $debug
        # this wrapper to raw_call_command handle reconnection on socket
        # error. We try to reconnect just one time, otherwise let the error
        # araise.
        connect_to_server if !@sock
        begin
            raw_call_command(argv)
        rescue Errno::ECONNRESET
            @sock.close
            connect_to_server
            raw_call_command(argv)
        end
    end

    def raw_call_command(argvp)
        pipeline = argvp[0].is_a?(Array)

        unless pipeline
          argvv = [argvp] 
        else
          argvv = argvp
        end

        command = ''

        argvv.each do |argv| 
          bulk = nil
          argv[0] = argv[0].to_s.downcase
          argv[0] = Aliases[argv[0]] if Aliases[argv[0]]
          if BulkCommands[argv[0]] and argv.length > 1
            bulk = argv[-1].to_s
            argv[-1] = bulk.length
          end
          command << argv.join(' ') + "\r\n"
          command << bulk + "\r\n" if bulk
        end
        
        @sock.write(command)

        results = argvv.map do |argv|
          processor = ReplyProcessor[argv[0]]
          processor ? processor.call(read_reply) : read_reply
        end

        return pipeline ? results : results[0]
    end

    def select(*args)
        raise "SELECT not allowed, use the :db option when creating the object"
    end

    def [](key)
        get(key)
    end

    def []=(key,value)
        set(key,value)
    end

    def set(key, value, expiry=nil)
      call_command([:set, key, value]) 
      expire(key, expiry) unless expiry.nil?
    end

    def sort(key, opts={})
        cmd = []
        cmd << "SORT #{key}"
        cmd << "BY #{opts[:by]}" if opts[:by]
        cmd << "GET #{[opts[:get]].flatten * ' GET '}" if opts[:get]
        cmd << "#{opts[:order]}" if opts[:order]
        cmd << "LIMIT #{opts[:limit].join(' ')}" if opts[:limit]
        call_command(cmd)
    end

    def incr(key,increment=nil)
        call_command(increment ? ["incrby",key,increment] :  ["incr",key])
    end

    def decr(key,decrement=nil)
        call_command(decrement ? ["decrby",key,decrement] :  ["decr",key])
    end
    
    # Ruby defines a now deprecated type method so we need to override it here
    # since it will never hit method_missing
    def type(key)
        call_command(['type', key])
    end

    def quit
        call_command(['quit'])
    rescue Errno::ECONNRESET
    end

    def pipelined(&block)
        pipeline = Pipeline.new self
        yield pipeline
        pipeline.execute
    end

    def read_reply
        # We read the first byte using read() mainly because gets() is
        # immune to raw socket timeouts.
        begin
            rtype = @sock.read(1)
        rescue Errno::EAGAIN
            # We want to make sure it reconnects on the next command after the
            # timeout. Otherwise the server may reply in the meantime leaving
            # the protocol in a desync status.
            @sock = nil
            raise Errno::EAGAIN, "Timeout reading from the socket"
        end

        raise Errno::ECONNRESET,"Connection lost" if !rtype
        line = @sock.gets
        case rtype
        when "-"
            raise "-"+line.strip
        when "+"
            line.strip
        when ":"
            line.to_i
        when "$"
            bulklen = line.to_i
            return nil if bulklen == -1
            data = @sock.read(bulklen)
            @sock.read(2) # CRLF
            data
        when "*"
            objects = line.to_i
            return nil if bulklen == -1
            res = []
            objects.times {
                res << read_reply
            }
            res
        else
            raise "Protocol error, got '#{rtype}' as initial reply byte"
        end
    end
end
