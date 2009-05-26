require 'socket'
require 'set'
require File.join(File.dirname(__FILE__),'server')
require File.join(File.dirname(__FILE__),'pipeline')

class RedisError < StandardError
end
class RedisRenameError < StandardError
end

class Redis
  ERR = "-".freeze
  OK = 'OK'.freeze
  PONG = 'PONG'.freeze
  SINGLE = '+'.freeze
  BULK   = '$'.freeze
  MULTI  = '*'.freeze
  INT    = ':'.freeze
  
  attr_reader :server
  
  def initialize(opts={})
    @opts = {:host => 'localhost', :port => '6379', :db => 0}.merge(opts)
    $debug = @opts[:debug]
    @db = @opts[:db]
    @server = Server.new(@opts[:host], @opts[:port], (@opts[:timeout]||10))
  end

  def pipelined
    pipeline = Pipeline.new(self)
    yield pipeline
    pipeline.finish
  end

  def to_s
    "#{host}:#{port} -> #{@db}"
  end
  
  def port
    @opts[:port]
  end
  
  def host
    @opts[:host]
  end
  
  def quit
    execute_command("QUIT\r\n", true)
  end

  def ping
    execute_command("PING\r\n") == PONG
  end

  def select_db(index)
    @db = index
    execute_command("SELECT #{index}\r\n")
  end
  
  def flush_db
    execute_command("FLUSHDB\r\n") == OK
  end    

  def flush_all
    puts "Warning!\nFlushing *ALL* databases!\n5 Seconds to Hit ^C!"
    trap('INT') {quit; return false}
    sleep 5
    execute_command("FLUSHALL\r\n") == OK
  end

  def last_save
    execute_command("LASTSAVE\r\n").to_i
  end
  
  def bgsave
    execute_command("BGSAVE\r\n") == OK
  end  
    
  def info
   info = {}
   x = execute_command("INFO\r\n")
   x.each_line do |kv|
     k,v = kv.split(':', 2)
     k,v = k.chomp, v = v.chomp
     info[k.to_sym] = v
   end
   info
  end
  
  def keys(glob)
    execute_command("KEYS #{glob}\r\n").split(' ')
  end

  def rename!(oldkey, newkey)
    execute_command("RENAME #{oldkey} #{newkey}\r\n")
  end  
  
  def rename(oldkey, newkey)
    case execute_command("RENAMENX #{oldkey} #{newkey}\r\n")
    when -1
      raise RedisRenameError, "source key: #{oldkey} does not exist"
    when 0
      raise RedisRenameError, "target key: #{oldkey} already exists"
    when -3
      raise RedisRenameError, "source and destination keys are the same"
    when 1
      true
    end
  end  
  
  def key?(key)
    execute_command("EXISTS #{key}\r\n") == 1
  end  
  
  def delete(key)
    execute_command("DEL #{key}\r\n") == 1
  end  
  
  def [](key)
    get(key)
  end

  def get(key)
    execute_command("GET #{key}\r\n")
  end
  
  def mget(*keys)
    execute_command("MGET #{keys.join(' ')}\r\n")
  end

  def incr(key, increment=nil)
    if increment
      execute_command("INCRBY #{key} #{increment}\r\n")
    else
      execute_command("INCR #{key}\r\n")
    end    
  end

  def decr(key, decrement=nil)
    if decrement
      execute_command("DECRBY #{key} #{decrement}\r\n")
    else
      execute_command("DECR #{key}\r\n")
    end    
  end
  
  def randkey
    execute_command("RANDOMKEY\r\n")
  end

  def list_length(key)
    case i = execute_command("LLEN #{key}\r\n")
    when -2
      raise RedisError, "key: #{key} does not hold a list value"
    else
      i
    end
  end

  def type?(key)
    execute_command("TYPE #{key}\r\n")
  end
  
  def push_tail(key, val)
    execute_command("RPUSH #{key} #{value_to_wire(val)}\r\n")
  end      

  def push_head(key, val)
    execute_command("LPUSH #{key} #{value_to_wire(val)}\r\n")
  end
  
  def pop_head(key)
    execute_command("LPOP #{key}\r\n")
  end

  def pop_tail(key)
    execute_command("RPOP #{key}\r\n")
  end    

  def list_set(key, index, val)
    execute_command("LSET #{key} #{index} #{value_to_wire(val)}\r\n") == OK
  end

  def list_range(key, start, ending)
    execute_command("LRANGE #{key} #{start} #{ending}\r\n")
  end

  def list_trim(key, start, ending)
    execute_command("LTRIM #{key} #{start} #{ending}\r\n")
  end

  def list_index(key, index)
    execute_command("LINDEX #{key} #{index}\r\n")
  end

  def list_rm(key, count, val)
    case num = execute_command("LREM #{key} #{count} #{value_to_wire(val)}\r\n")
    when -1
      raise RedisError, "key: #{key} does not exist"
    when -2
      raise RedisError, "key: #{key} does not hold a list value"
    else
      num
    end
  end 

  def set_add(key, member)
    case execute_command("SADD #{key} #{value_to_wire(member)}\r\n")
    when 1
      true
    when 0
      false
    when -2
      raise RedisError, "key: #{key} contains a non set value"
    end
  end

  def set_delete(key, member)
    case execute_command("SREM #{key} #{value_to_wire(member)}\r\n")
    when 1
      true
    when 0
      false
    when -2
      raise RedisError, "key: #{key} contains a non set value"
    end
  end

  def set_count(key)
    case i = execute_command("SCARD #{key}\r\n")
    when -2
      raise RedisError, "key: #{key} contains a non set value"
    else
      i
    end
  end

  def set_member?(key, member)
    case execute_command("SISMEMBER #{key} #{value_to_wire(member)}\r\n")
    when 1
      true
    when 0
      false
    when -2
      raise RedisError, "key: #{key} contains a non set value"
    end
  end

  def set_members(key)
    Set.new(execute_command("SMEMBERS #{key}\r\n"))
  end

  def set_intersect(*keys)
    Set.new(execute_command("SINTER #{keys.join(' ')}\r\n"))
  end

  def set_inter_store(destkey, *keys)
    execute_command("SINTERSTORE #{destkey} #{keys.join(' ')}\r\n")
  end
  
  def set_union(*keys)
    Set.new(execute_command("SUNION #{keys.join(' ')}\r\n"))
  end

  def set_union_store(destkey, *keys)
    execute_command("SUNIONSTORE #{destkey} #{keys.join(' ')}\r\n")
  end
  
  def set_diff(*keys)
    Set.new(execute_command("SDIFF #{keys.join(' ')}\r\n"))
  end

  def set_diff_store(destkey, *keys)
    execute_command("SDIFFSTORE #{destkey} #{keys.join(' ')}\r\n")
  end

  def set_move(srckey, destkey, member)
    execute_command("SMOVE #{srckey} #{destkey} #{value_to_wire(member)}\r\n") == 1
  end

  def sort(key, opts={})
    cmd = "SORT #{key}"
    cmd << " BY #{opts[:by]}" if opts[:by]
    cmd << " GET #{[opts[:get]].flatten * ' GET '}" if opts[:get]
    cmd << " INCR #{opts[:incr]}" if opts[:incr]
    cmd << " DEL #{opts[:del]}" if opts[:del]
    cmd << " DECR #{opts[:decr]}" if opts[:decr]
    cmd << " #{opts[:order]}" if opts[:order]
    cmd << " LIMIT #{opts[:limit].join(' ')}" if opts[:limit]
    cmd << "\r\n"
    execute_command(cmd)
  end
  
  def []=(key, val)
    set(key,val)
  end
  
  def set(key, val, expiry=nil)
    s = execute_command("SET #{key} #{value_to_wire(val)}\r\n") == OK
    return expire(key, expiry) if s && expiry
    s
  end

  def dbsize
    execute_command("DBSIZE\r\n")
  end

  def expire(key, expiry=nil)
    execute_command("EXPIRE #{key} #{expiry}\r\n") == 1
  end

  def set_unless_exists(key, val)
    execute_command("SETNX #{key} #{value_to_wire(val)}\r\n") == 1
  end  
  
  def bulk_reply
    begin
      x = read
      puts "bulk_reply read value is #{x.inspect}" if $debug
      return x
    rescue => e
      puts "error in bulk_reply #{e}" if $debug
      nil
    end
  end
  
  def write(data)
    puts "writing: #{data}" if $debug
    @socket.write(data)
  end
  
  def read(length = 0)
    length = read_proto unless length > 0
    res = @socket.read(length)
    puts "read is #{res.inspect}" if $debug
    res
  end
      
  def multi_bulk
    res = read_proto
    puts "mb res is #{res.inspect}" if $debug
    list = []
    Integer(res).times do
      vf = get_response
      puts "curren vf is #{vf.inspect}" if $debug
      list << vf
      puts "current list is #{list.inspect}" if $debug
    end
    list
  end
   
  def get_reply
    begin
      r = read(1)
      raise RedisError if (r == "\r" || r == "\n")
    rescue RedisError
      retry
    end
    r
  end
   
  def status_code_reply
    begin
      res = read_proto  
      if res.index('-') == 0          
        raise RedisError, res
      else          
        true
      end
    rescue RedisError
       raise RedisError
    end
  end
 
  def execute_command(command, ignore_response=false)
    ss = server.socket
    unless ss.object_id == @socket.object_id
      @socket = ss
      puts "Socket changed, selecting DB" if $debug
      unless command[0..6] == 'SELECT'
      #BTM - Ugh- DRY but better than infinite recursion
        write("SELECT #{@db}\r\n") 
        get_response
      end
    end 
    write(command)
    get_response unless ignore_response
  rescue Errno::ECONNRESET, Errno::EPIPE, NoMethodError, Timeout::Error => e
    raise RedisError, "Connection error"
  end

  def get_response
    rtype = get_reply
    puts "reply_type is #{rtype.inspect}" if $debug
    case rtype
    when SINGLE
      single_line
    when BULK
      bulk_reply
    when MULTI
      multi_bulk
    when INT
      integer_reply
    when ERR
      raise RedisError, single_line
    else
      raise RedisError, "Unknown response.."
    end
  end
  
  def integer_reply
    Integer(read_proto)
  end
  
  def single_line
    buff = ""
    while buff[-2..-1] != "\r\n"
      buff << read(1)
    end
    puts "single_line value is #{buff[0..-3].inspect}" if $debug
    buff[0..-3]
  end
  
  def read_socket
    begin
      socket = @server.socket
      while res = socket.read(8096)
        break if res.size != 8096
      end
    #Timeout or server down
    rescue Errno::ECONNRESET, Errno::EPIPE, Errno::ECONNREFUSED => e
      server.close
      puts "Client (#{server.inspect}) disconnected from server: #{e.inspect}\n" if $debug
      retry
    rescue Timeout::Error => e
    #BTM - Ignore this error so we don't go into an endless loop
      puts "Client (#{server.inspect}) Timeout\n" if $debug
    #Server down
    rescue NoMethodError => e
      puts "Client (#{server.inspect}) tryin server that is down: #{e.inspect}\n Dying!" if $debug
      raise Errno::ECONNREFUSED
      #exit
    end
  end
  
  def read_proto
    res = @socket.readline
    x = res.chomp
    puts "read_proto is #{x.inspect}\n\n" if $debug
    x.to_i
  end

  private
  def value_to_wire(value)
    value_str = value.to_s
    if value_str.respond_to?(:bytesize)
      value_size = value_str.bytesize
    else
      value_size = value_str.size
    end
    "#{value_size}\r\n#{value_str}"
  end
  
end
