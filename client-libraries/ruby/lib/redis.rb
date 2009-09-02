require 'socket'
require File.join(File.dirname(__FILE__),'pipeline')

begin
  if RUBY_VERSION >= '1.9'
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
  OK      = "OK".freeze
  MINUS    = "-".freeze
  PLUS     = "+".freeze
  COLON    = ":".freeze
  DOLLAR   = "$".freeze
  ASTERISK = "*".freeze

  BULK_COMMANDS = {
    "set"       => true,
    "setnx"     => true,
    "rpush"     => true,
    "lpush"     => true,
    "lset"      => true,
    "lrem"      => true,
    "sadd"      => true,
    "srem"      => true,
    "sismember" => true,
    "echo"      => true,
    "getset"    => true,
    "smove"     => true
  }

  BOOLEAN_PROCESSOR = lambda{|r| r == 1 }

  REPLY_PROCESSOR = {
    "exists"    => BOOLEAN_PROCESSOR,
    "sismember" => BOOLEAN_PROCESSOR,
    "sadd"      => BOOLEAN_PROCESSOR,
    "srem"      => BOOLEAN_PROCESSOR,
    "smove"     => BOOLEAN_PROCESSOR,
    "move"      => BOOLEAN_PROCESSOR,
    "setnx"     => BOOLEAN_PROCESSOR,
    "del"       => BOOLEAN_PROCESSOR,
    "renamenx"  => BOOLEAN_PROCESSOR,
    "expire"    => BOOLEAN_PROCESSOR,
    "keys"      => lambda{|r| r.split(" ")},
    "info"      => lambda{|r|
      info = {}
      r.each_line {|kv|
        k,v = kv.split(":",2).map{|x| x.chomp}
        info[k.to_sym] = v
      }
      info
    }
  }

  ALIASES = {
    "flush_db"             => "flushdb",
    "flush_all"            => "flushall",
    "last_save"            => "lastsave",
    "key?"                 => "exists",
    "delete"               => "del",
    "randkey"              => "randomkey",
    "list_length"          => "llen",
    "push_tail"            => "rpush",
    "push_head"            => "lpush",
    "pop_tail"             => "rpop",
    "pop_head"             => "lpop",
    "list_set"             => "lset",
    "list_range"           => "lrange",
    "list_trim"            => "ltrim",
    "list_index"           => "lindex",
    "list_rm"              => "lrem",
    "set_add"              => "sadd",
    "set_delete"           => "srem",
    "set_count"            => "scard",
    "set_member?"          => "sismember",
    "set_members"          => "smembers",
    "set_intersect"        => "sinter",
    "set_intersect_store"  => "sinterstore",
    "set_inter_store"      => "sinterstore",
    "set_union"            => "sunion",
    "set_union_store"      => "sunionstore",
    "set_diff"             => "sdiff",
    "set_diff_store"       => "sdiffstore",
    "set_move"             => "smove",
    "set_unless_exists"    => "setnx",
    "rename_unless_exists" => "renamenx",
    "type?"                => "type"
  }

  DISABLED_COMMANDS = {
    "monitor" => true,
    "sync"    => true
  }

  def initialize(options = {})
    @host    =  options[:host]    || '127.0.0.1'
    @port    = (options[:port]    || 6379).to_i
    @db      = (options[:db]      || 0).to_i
    @timeout = (options[:timeout] || 5).to_i
    @password = options[:password]
    @logger  =  options[:logger]

    @logger.info { self.to_s } if @logger
    connect_to_server
  end

  def to_s
    "Redis Client connected to #{server} against DB #{@db}"
  end

  def server
    "#{@host}:#{@port}"
  end

  def connect_to_server
    @sock = connect_to(@host, @port, @timeout == 0 ? nil : @timeout)
    call_command(["auth",@password]) if @password
    call_command(["select",@db]) unless @db == 0
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
      secs   = Integer(timeout)
      usecs  = Integer((timeout - secs) * 1_000_000)
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
    @logger.debug { argv.inspect } if @logger

    # this wrapper to raw_call_command handle reconnection on socket
    # error. We try to reconnect just one time, otherwise let the error
    # araise.
    connect_to_server if !@sock

    begin
      raw_call_command(argv.dup)
    rescue Errno::ECONNRESET, Errno::EPIPE
      @sock.close
      @sock = nil
      connect_to_server
      raw_call_command(argv.dup)
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
      argv[0] = ALIASES[argv[0]] if ALIASES[argv[0]]
      raise "#{argv[0]} command is disabled" if DISABLED_COMMANDS[argv[0]]
      if BULK_COMMANDS[argv[0]] and argv.length > 1
        bulk = argv[-1].to_s
        argv[-1] = bulk.respond_to?(:bytesize) ? bulk.bytesize : bulk.size
      end
      command << "#{argv.join(' ')}\r\n"
      command << "#{bulk}\r\n" if bulk
    end

    @sock.write(command)

    results = argvv.map do |argv|
      processor = REPLY_PROCESSOR[argv[0]]
      processor ? processor.call(read_reply) : read_reply
    end

    return pipeline ? results : results[0]
  end

  def select(*args)
    raise "SELECT not allowed, use the :db option when creating the object"
  end

  def [](key)
    self.get(key)
  end

  def []=(key,value)
    set(key,value)
  end

  def set(key, value, expiry=nil)
    s = call_command([:set, key, value]) == OK
    expire(key, expiry) if s && expiry
    s
  end

  def sort(key, options = {})
    cmd = ["SORT"]
    cmd << key
    cmd << "BY #{options[:by]}" if options[:by]
    cmd << "GET #{[options[:get]].flatten * ' GET '}" if options[:get]
    cmd << "#{options[:order]}" if options[:order]
    cmd << "LIMIT #{options[:limit].join(' ')}" if options[:limit]
    call_command(cmd)
  end

  def incr(key, increment = nil)
    call_command(increment ? ["incrby",key,increment] : ["incr",key])
  end

  def decr(key,decrement = nil)
    call_command(decrement ? ["decrby",key,decrement] : ["decr",key])
  end

  # Similar to memcache.rb's #get_multi, returns a hash mapping
  # keys to values.
  def mapped_mget(*keys)
    mget(*keys).inject({}) do |hash, value|
      key = keys.shift
      value.nil? ? hash : hash.merge(key => value)
    end
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
    when MINUS
      raise MINUS + line.strip
    when PLUS
      line.strip
    when COLON
      line.to_i
    when DOLLAR
      bulklen = line.to_i
      return nil if bulklen == -1
      data = @sock.read(bulklen)
      @sock.read(2) # CRLF
      data
    when ASTERISK
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
