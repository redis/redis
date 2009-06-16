require File.dirname(__FILE__) + '/spec_helper'

class Foo
  attr_accessor :bar
  def initialize(bar)
    @bar = bar
  end
  
  def ==(other)
    @bar == other.bar
  end
end  

describe "redis" do
  before(:all) do
    # use database 15 for testing so we dont accidentally step on you real data
    @r = Redis.new :db => 15
  end

  before(:each) do
    @r['foo'] = 'bar'
  end

  after(:each) do
    @r.keys('*').each {|k| @r.del k}
  end  

  after(:all) do
    @r.quit
  end  

  it "should be able connect without a timeout" do
    lambda { Redis.new :timeout => 0 }.should_not raise_error
  end

  it "should be able to PING" do
    @r.ping.should == 'PONG' 
  end

  it "should be able to GET a key" do
    @r['foo'].should == 'bar'
  end
  
  it "should be able to SET a key" do
    @r['foo'] = 'nik'
    @r['foo'].should == 'nik'
  end
  
  it "should properly handle trailing newline characters" do
    @r['foo'] = "bar\n"
    @r['foo'].should == "bar\n"
  end
  
  it "should store and retrieve all possible characters at the beginning and the end of a string" do
    (0..255).each do |char_idx|
      string = "#{char_idx.chr}---#{char_idx.chr}"
      @r['foo'] = string
      @r['foo'].should == string
    end
  end
  
  it "should be able to SET a key with an expiry" do
    @r.set('foo', 'bar', 1)
    @r['foo'].should == 'bar'
    sleep 2
    @r['foo'].should == nil
  end

  it "should be able to return a TTL for a key" do
    @r.set('foo', 'bar', 1)
    @r.ttl('foo').should == 1
  end

  it "should be able to SETNX" do
    @r['foo'] = 'nik'
    @r['foo'].should == 'nik'
    @r.setnx 'foo', 'bar'
    @r['foo'].should == 'nik'
  end
  #
  it "should be able to GETSET" do
   @r.getset('foo', 'baz').should == 'bar'
   @r['foo'].should == 'baz'
  end
  # 
  it "should be able to INCR a key" do
    @r.del('counter')
    @r.incr('counter').should == 1
    @r.incr('counter').should == 2
    @r.incr('counter').should == 3
  end
  #
  it "should be able to INCRBY a key" do
    @r.del('counter')
    @r.incrby('counter', 1).should == 1
    @r.incrby('counter', 2).should == 3
    @r.incrby('counter', 3).should == 6
  end
  #
  it "should be able to DECR a key" do
    @r.del('counter')
    @r.incr('counter').should == 1
    @r.incr('counter').should == 2
    @r.incr('counter').should == 3
    @r.decr('counter').should == 2
    @r.decr('counter', 2).should == 0
  end
  # 
  it "should be able to RANDKEY" do
    @r.randkey.should_not be_nil
  end
  # 
  it "should be able to RENAME a key" do
    @r.del 'foo'
    @r.del'bar'
    @r['foo'] = 'hi'
    @r.rename 'foo', 'bar'
    @r['bar'].should == 'hi'
  end
  # 
  it "should be able to RENAMENX a key" do
    @r.del 'foo'
    @r.del 'bar'
    @r['foo'] = 'hi'
    @r['bar'] = 'ohai'
    @r.renamenx 'foo', 'bar'
    @r['bar'].should == 'ohai'
  end
  #
  it "should be able to get DBSIZE of the database" do
    @r.delete 'foo'
    dbsize_without_foo = @r.dbsize
    @r['foo'] = 0
    dbsize_with_foo = @r.dbsize

    dbsize_with_foo.should == dbsize_without_foo + 1
  end
  #
  it "should be able to EXPIRE a key" do
    @r['foo'] = 'bar'
    @r.expire 'foo', 1
    @r['foo'].should == "bar"
    sleep 2
    @r['foo'].should == nil
  end
  #
  it "should be able to EXISTS" do
    @r['foo'] = 'nik'
    @r.exists('foo').should be_true
    @r.del 'foo'
    @r.exists('foo').should be_false
  end
  # 
  it "should be able to KEYS" do
    @r.keys("f*").each { |key| @r.del key }
    @r['f'] = 'nik'
    @r['fo'] = 'nak'
    @r['foo'] = 'qux'
    @r.keys("f*").sort.should == ['f','fo', 'foo'].sort
  end
  #
  it "should be able to return a random key (RANDOMKEY)" do
    3.times { @r.exists(@r.randomkey).should be_true }
  end
  #BTM - TODO 
  it "should be able to check the TYPE of a key" do
    @r['foo'] = 'nik'
    @r.type('foo').should == "string"
    @r.del 'foo'
    @r.type('foo').should == "none"
  end
  # 
  it "should be able to push to the head of a list (LPUSH)" do
    @r.lpush "list", 'hello'
    @r.lpush "list", 42
    @r.type('list').should == "list"
    @r.llen('list').should == 2
    @r.lpop('list').should == '42'
  end
  # 
  it "should be able to push to the tail of a list (RPUSH)" do
    @r.rpush "list", 'hello'
    @r.type('list').should == "list"
    @r.llen('list').should == 1
  end
  # 
  it "should be able to pop the tail of a list (RPOP)" do
    @r.rpush "list", 'hello'
    @r.rpush"list", 'goodbye'
    @r.type('list').should == "list"
    @r.llen('list').should == 2
    @r.rpop('list').should == 'goodbye'
  end
  # 
  it "should be able to pop the head of a list (LPOP)" do
    @r.rpush "list", 'hello'
    @r.rpush "list", 'goodbye'
    @r.type('list').should == "list"
    @r.llen('list').should == 2
    @r.lpop('list').should == 'hello'
  end
  # 
  it "should be able to get the length of a list (LLEN)" do
    @r.rpush "list", 'hello'
    @r.rpush "list", 'goodbye'
    @r.type('list').should == "list"
    @r.llen('list').should == 2
  end
  # 
  it "should be able to get a range of values from a list (LRANGE)" do
    @r.rpush "list", 'hello'
    @r.rpush "list", 'goodbye'
    @r.rpush "list", '1'
    @r.rpush "list", '2'
    @r.rpush "list", '3'
    @r.type('list').should == "list"
    @r.llen('list').should == 5
    @r.lrange('list', 2, -1).should == ['1', '2', '3']
  end
  # 
  it "should be able to trim a list (LTRIM)" do
    @r.rpush "list", 'hello'
    @r.rpush "list", 'goodbye'
    @r.rpush "list", '1'
    @r.rpush "list", '2'
    @r.rpush "list", '3'
    @r.type('list').should == "list"
    @r.llen('list').should == 5
    @r.ltrim 'list', 0, 1
    @r.llen('list').should == 2
    @r.lrange('list', 0, -1).should == ['hello', 'goodbye']
  end
  # 
  it "should be able to get a value by indexing into a list (LINDEX)" do
    @r.rpush "list", 'hello'
    @r.rpush "list", 'goodbye'
    @r.type('list').should == "list"
    @r.llen('list').should == 2
    @r.lindex('list', 1).should == 'goodbye'
  end
  # 
  it "should be able to set a value by indexing into a list (LSET)" do
    @r.rpush "list", 'hello'
    @r.rpush "list", 'hello'
    @r.type('list').should == "list"
    @r.llen('list').should == 2
    @r.lset('list', 1, 'goodbye').should == 'OK'
    @r.lindex('list', 1).should == 'goodbye'
  end
  # 
  it "should be able to remove values from a list (LREM)" do
    @r.rpush "list", 'hello'
    @r.rpush "list", 'goodbye'
    @r.type('list').should == "list"
    @r.llen('list').should == 2
    @r.lrem('list', 1, 'hello').should == 1
    @r.lrange('list', 0, -1).should == ['goodbye']
  end
  # 
  it "should be able add members to a set (SADD)" do
    @r.sadd "set", 'key1'
    @r.sadd "set", 'key2'
    @r.type('set').should == "set"
    @r.scard('set').should == 2
    @r.smembers('set').sort.should == ['key1', 'key2'].sort
  end
  # 
  it "should be able delete members to a set (SREM)" do
    @r.sadd "set", 'key1'
    @r.sadd "set", 'key2'
    @r.type('set').should == "set"
    @r.scard('set').should == 2
    @r.smembers('set').sort.should == ['key1', 'key2'].sort
    @r.srem('set', 'key1')
    @r.scard('set').should == 1
    @r.smembers('set').should == ['key2']
  end
  # 
  it "should be able count the members of a set (SCARD)" do
    @r.sadd "set", 'key1'
    @r.sadd "set", 'key2'
    @r.type('set').should == "set"
    @r.scard('set').should == 2
  end
  # 
  it "should be able test for set membership (SISMEMBER)" do
    @r.sadd "set", 'key1'
    @r.sadd "set", 'key2'
    @r.type('set').should == "set"
    @r.scard('set').should == 2
    @r.sismember('set', 'key1').should be_true
    @r.sismember('set', 'key2').should be_true
    @r.sismember('set', 'notthere').should be_false
  end
  # 
  it "should be able to do set intersection (SINTER)" do
    @r.sadd "set", 'key1'
    @r.sadd "set", 'key2'
    @r.sadd "set2", 'key2'
    @r.sinter('set', 'set2').should == ['key2']
  end
  # 
  it "should be able to do set intersection and store the results in a key (SINTERSTORE)" do
    @r.sadd "set", 'key1'
    @r.sadd "set", 'key2'
    @r.sadd "set2", 'key2'
    @r.sinterstore('newone', 'set', 'set2').should == 1
    @r.smembers('newone').should == ['key2']
  end
  #
  it "should be able to do set union (SUNION)" do
    @r.sadd "set", 'key1'
    @r.sadd "set", 'key2'
    @r.sadd "set2", 'key2'
    @r.sadd "set2", 'key3'
    @r.sunion('set', 'set2').sort.should == ['key1','key2','key3'].sort
  end
  # 
  it "should be able to do set union and store the results in a key (SUNIONSTORE)" do
    @r.sadd "set", 'key1'
    @r.sadd "set", 'key2'
    @r.sadd "set2", 'key2'
    @r.sadd "set2", 'key3'
    @r.sunionstore('newone', 'set', 'set2').should == 3
    @r.smembers('newone').sort.should == ['key1','key2','key3'].sort
  end
  # 
  it "should be able to do set difference (SDIFF)" do
     @r.sadd "set", 'a'
     @r.sadd "set", 'b'
     @r.sadd "set2", 'b'
     @r.sadd "set2", 'c'
     @r.sdiff('set', 'set2').should == ['a']
   end
  # 
  it "should be able to do set difference and store the results in a key (SDIFFSTORE)" do
     @r.sadd "set", 'a'
     @r.sadd "set", 'b'
     @r.sadd "set2", 'b'
     @r.sadd "set2", 'c'
     @r.sdiffstore('newone', 'set', 'set2')
     @r.smembers('newone').should == ['a']
   end
  # 
  it "should be able move elements from one set to another (SMOVE)" do
    @r.sadd 'set1', 'a'
    @r.sadd 'set1', 'b'
    @r.sadd 'set2', 'x'
    @r.smove('set1', 'set2', 'a').should be_true 
    @r.sismember('set2', 'a').should be_true
    @r.delete('set1')
  end
  #
  it "should be able to do crazy SORT queries" do
    @r['dog_1'] = 'louie'
    @r.rpush 'dogs', 1
    @r['dog_2'] = 'lucy'
    @r.rpush 'dogs', 2
    @r['dog_3'] = 'max'
    @r.rpush 'dogs', 3
    @r['dog_4'] = 'taj'
    @r.rpush 'dogs', 4
    @r.sort('dogs', :get => 'dog_*', :limit => [0,1]).should == ['louie']
    @r.sort('dogs', :get => 'dog_*', :limit => [0,1], :order => 'desc alpha').should == ['taj']
  end

  it "should be able to handle array of :get using SORT" do
    @r['dog:1:name'] = 'louie'
    @r['dog:1:breed'] = 'mutt'
    @r.rpush 'dogs', 1
    @r['dog:2:name'] = 'lucy'
    @r['dog:2:breed'] = 'poodle'
    @r.rpush 'dogs', 2
    @r['dog:3:name'] = 'max'
    @r['dog:3:breed'] = 'hound'
    @r.rpush 'dogs', 3
    @r['dog:4:name'] = 'taj'
    @r['dog:4:breed'] = 'terrier'
    @r.rpush 'dogs', 4
    @r.sort('dogs', :get => ['dog:*:name', 'dog:*:breed'], :limit => [0,1]).should == ['louie', 'mutt']
    @r.sort('dogs', :get => ['dog:*:name', 'dog:*:breed'], :limit => [0,1], :order => 'desc alpha').should == ['taj', 'terrier']
  end
  # 
  it "should provide info (INFO)" do
    [:last_save_time, :redis_version, :total_connections_received, :connected_clients, :total_commands_processed, :connected_slaves, :uptime_in_seconds, :used_memory, :uptime_in_days, :changes_since_last_save].each do |x|
    @r.info.keys.should include(x)
    end
  end
  # 
  it "should be able to flush the database (FLUSHDB)" do
    @r['key1'] = 'keyone'
    @r['key2'] = 'keytwo'
    @r.keys('*').sort.should == ['foo', 'key1', 'key2'].sort #foo from before
    @r.flushdb
    @r.keys('*').should == []
  end
  #
  it "should raise exception when manually try to change the database" do
    lambda { @r.select(0) }.should raise_error
  end
  #
  it "should be able to provide the last save time (LASTSAVE)" do
    savetime = @r.lastsave
    Time.at(savetime).class.should == Time
    Time.at(savetime).should <= Time.now
  end
  
  it "should be able to MGET keys" do
    @r['foo'] = 1000
    @r['bar'] = 2000
    @r.mget('foo', 'bar').should == ['1000', '2000']
    @r.mget('foo', 'bar', 'baz').should == ['1000', '2000', nil]
  end
  
  it "should bgsave" do
    @r.bgsave.should == 'OK'
  end
  
  it "should should be able to ECHO" do
    @r.echo("message in a bottle\n").should == "message in a bottle\n"
  end

  it "should handle multiple servers" do
    require 'dist_redis'
    @r = DistRedis.new(:hosts=> ['localhost:6379', '127.0.0.1:6379'], :db => 15)

    100.times do |idx|
      @r[idx] = "foo#{idx}"
    end

    100.times do |idx|
      @r[idx].should == "foo#{idx}"
    end
  end

  it "should be able to pipeline writes" do
    @r.pipelined do |pipeline|
      pipeline.lpush 'list', "hello"
      pipeline.lpush 'list', 42
    end
    
    @r.type('list').should == "list"
    @r.llen('list').should == 2
    @r.lpop('list').should == '42'
  end
  
end
