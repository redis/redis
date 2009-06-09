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
    @r.keys('*').each {|k| @r.delete k}
  end  

  after(:all) do
    @r.quit
  end  

  it 'should be able to PING' do
    @r.ping.should == true
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
  
  it "should be able to SETNX(set_unless_exists)" do
    @r['foo'] = 'nik'
    @r['foo'].should == 'nik'
    @r.set_unless_exists 'foo', 'bar'
    @r['foo'].should == 'nik'
  end
  # 
  it "should be able to INCR(increment) a key" do
    @r.delete('counter')
    @r.incr('counter').should == 1
    @r.incr('counter').should == 2
    @r.incr('counter').should == 3
  end
  # 
  it "should be able to DECR(decrement) a key" do
    @r.delete('counter')
    @r.incr('counter').should == 1
    @r.incr('counter').should == 2
    @r.incr('counter').should == 3
    @r.decr('counter').should == 2
    @r.decr('counter', 2).should == 0
  end
  # 
  it "should be able to RANDKEY(return a random key)" do
    @r.randkey.should_not be_nil
  end
  # 
  it "should be able to RENAME a key" do
    @r.delete 'foo'
    @r.delete 'bar'
    @r['foo'] = 'hi'
    @r.rename! 'foo', 'bar'
    @r['bar'].should == 'hi'
  end
  # 
  it "should be able to RENAMENX(rename unless the new key already exists) a key" do
    @r.delete 'foo'
    @r.delete 'bar'
    @r['foo'] = 'hi'
    @r['bar'] = 'ohai'
    lambda {@r.rename 'foo', 'bar'}.should raise_error(RedisRenameError)
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
    @r.expire('foo', 1)
    @r['foo'].should == "bar"
    sleep 2
    @r['foo'].should == nil
  end
  #
  it "should be able to EXISTS(check if key exists)" do
    @r['foo'] = 'nik'
    @r.key?('foo').should be_true
    @r.delete 'foo'
    @r.key?('foo').should be_false
  end
  # 
  it "should be able to KEYS(glob for keys)" do
    @r.keys("f*").each do |key|
      @r.delete key
    end  
    @r['f'] = 'nik'
    @r['fo'] = 'nak'
    @r['foo'] = 'qux'
    @r.keys("f*").sort.should == ['f','fo', 'foo'].sort
  end
  # 
  it "should be able to check the TYPE of a key" do
    @r['foo'] = 'nik'
    @r.type?('foo').should == "string"
    @r.delete 'foo'
    @r.type?('foo').should == "none"
  end
  # 
  it "should be able to push to the head of a list" do
    @r.push_head "list", 'hello'
    @r.push_head "list", 42
    @r.type?('list').should == "list"
    @r.list_length('list').should == 2
    @r.pop_head('list').should == '42'
    @r.delete('list')
  end
  # 
  it "should be able to push to the tail of a list" do
    @r.push_tail "list", 'hello'
    @r.type?('list').should == "list"
    @r.list_length('list').should == 1
    @r.delete('list')
  end
  # 
  it "should be able to pop the tail of a list" do
    @r.push_tail "list", 'hello'
    @r.push_tail "list", 'goodbye'
    @r.type?('list').should == "list"
    @r.list_length('list').should == 2
    @r.pop_tail('list').should == 'goodbye'
    @r.delete('list')
  end
  # 
  it "should be able to pop the head of a list" do
    @r.push_tail "list", 'hello'
    @r.push_tail "list", 'goodbye'
    @r.type?('list').should == "list"
    @r.list_length('list').should == 2
    @r.pop_head('list').should == 'hello'
    @r.delete('list')
  end
  # 
  it "should be able to get the length of a list" do
    @r.push_tail "list", 'hello'
    @r.push_tail "list", 'goodbye'
    @r.type?('list').should == "list"
    @r.list_length('list').should == 2
    @r.delete('list')
  end
  # 
  it "should be able to get a range of values from a list" do
    @r.push_tail "list", 'hello'
    @r.push_tail "list", 'goodbye'
    @r.push_tail "list", '1'
    @r.push_tail "list", '2'
    @r.push_tail "list", '3'
    @r.type?('list').should == "list"
    @r.list_length('list').should == 5
    @r.list_range('list', 2, -1).should == ['1', '2', '3']
    @r.delete('list')
  end
  # 
  it "should be able to trim a list" do
    @r.push_tail "list", 'hello'
    @r.push_tail "list", 'goodbye'
    @r.push_tail "list", '1'
    @r.push_tail "list", '2'
    @r.push_tail "list", '3'
    @r.type?('list').should == "list"
    @r.list_length('list').should == 5
    @r.list_trim 'list', 0, 1
    @r.list_length('list').should == 2
    @r.list_range('list', 0, -1).should == ['hello', 'goodbye']
    @r.delete('list')
  end
  # 
  it "should be able to get a value by indexing into a list" do
    @r.push_tail "list", 'hello'
    @r.push_tail "list", 'goodbye'
    @r.type?('list').should == "list"
    @r.list_length('list').should == 2
    @r.list_index('list', 1).should == 'goodbye'
    @r.delete('list')
  end
  # 
  it "should be able to set a value by indexing into a list" do
    @r.push_tail "list", 'hello'
    @r.push_tail "list", 'hello'
    @r.type?('list').should == "list"
    @r.list_length('list').should == 2
    @r.list_set('list', 1, 'goodbye').should be_true
    @r.list_index('list', 1).should == 'goodbye'
    @r.delete('list')
  end
  # 
  it "should be able to remove values from a list LREM" do
    @r.push_tail "list", 'hello'
    @r.push_tail "list", 'goodbye'
    @r.type?('list').should == "list"
    @r.list_length('list').should == 2
    @r.list_rm('list', 1, 'hello').should == 1
    @r.list_range('list', 0, -1).should == ['goodbye']
    @r.delete('list')
  end
  # 
  it "should be able add members to a set" do
    @r.set_add "set", 'key1'
    @r.set_add "set", 'key2'
    @r.type?('set').should == "set"
    @r.set_count('set').should == 2
    @r.set_members('set').sort.should == ['key1', 'key2'].sort
    @r.delete('set')
  end
  # 
  it "should be able delete members to a set" do
    @r.set_add "set", 'key1'
    @r.set_add "set", 'key2'
    @r.type?('set').should == "set"
    @r.set_count('set').should == 2
    @r.set_members('set').should == Set.new(['key1', 'key2'])
    @r.set_delete('set', 'key1')
    @r.set_count('set').should == 1
    @r.set_members('set').should == Set.new(['key2'])
    @r.delete('set')
  end
  # 
  it "should be able count the members of a set" do
    @r.set_add "set", 'key1'
    @r.set_add "set", 'key2'
    @r.type?('set').should == "set"
    @r.set_count('set').should == 2
    @r.delete('set')
  end
  # 
  it "should be able test for set membership" do
    @r.set_add "set", 'key1'
    @r.set_add "set", 'key2'
    @r.type?('set').should == "set"
    @r.set_count('set').should == 2
    @r.set_member?('set', 'key1').should be_true
    @r.set_member?('set', 'key2').should be_true
    @r.set_member?('set', 'notthere').should be_false
    @r.delete('set')
  end
  # 
  it "should be able to do set intersection" do
    @r.set_add "set", 'key1'
    @r.set_add "set", 'key2'
    @r.set_add "set2", 'key2'
    @r.set_intersect('set', 'set2').should == Set.new(['key2'])
    @r.delete('set')
  end
  # 
  it "should be able to do set intersection and store the results in a key" do
    @r.set_add "set", 'key1'
    @r.set_add "set", 'key2'
    @r.set_add "set2", 'key2'
    @r.set_inter_store('newone', 'set', 'set2').should == 'OK'
    @r.set_members('newone').should == Set.new(['key2'])
    @r.delete('set')
    @r.delete('set2')
  end
  #
  it "should be able to do set union" do
    @r.set_add "set", 'key1'
    @r.set_add "set", 'key2'
    @r.set_add "set2", 'key2'
    @r.set_add "set2", 'key3'
    @r.set_union('set', 'set2').should == Set.new(['key1','key2','key3'])
    @r.delete('set')
    @r.delete('set2')
  end
  # 
  it "should be able to do set union and store the results in a key" do
    @r.set_add "set", 'key1'
    @r.set_add "set", 'key2'
    @r.set_add "set2", 'key2'
    @r.set_add "set2", 'key3'
    @r.set_union_store('newone', 'set', 'set2').should == 'OK'
    @r.set_members('newone').should == Set.new(['key1','key2','key3'])
    @r.delete('set')
    @r.delete('set2')
  end
  # 
  it "should be able to do set difference" do
     @r.set_add "set", 'a'
     @r.set_add "set", 'b'
     @r.set_add "set2", 'b'
     @r.set_add "set2", 'c'
     @r.set_diff('set', 'set2').should == Set.new(['a'])
     @r.delete('set')
     @r.delete('set2')
   end
  # 
  it "should be able to do set difference and store the results in a key" do
     @r.set_add "set", 'a'
     @r.set_add "set", 'b'
     @r.set_add "set2", 'b'
     @r.set_add "set2", 'c'
     @r.set_diff_store('newone', 'set', 'set2')
     @r.set_members('newone').should == Set.new(['a'])
     @r.delete('set')
     @r.delete('set2')
   end
  # 
  it "should be able move elements from one set to another" do
    @r.set_add 'set1', 'a'
    @r.set_add 'set1', 'b'
    @r.set_add 'set2', 'x'
    @r.set_move('set1', 'set2', 'a').should == true
    @r.set_member?('set2', 'a').should == true
    @r.delete('set1')
  end
  #
  it "should be able to do crazy SORT queries" do
    @r['dog_1'] = 'louie'
    @r.push_tail 'dogs', 1
    @r['dog_2'] = 'lucy'
    @r.push_tail 'dogs', 2
    @r['dog_3'] = 'max'
    @r.push_tail 'dogs', 3
    @r['dog_4'] = 'taj'
    @r.push_tail 'dogs', 4
    @r.sort('dogs', :get => 'dog_*', :limit => [0,1]).should == ['louie']
    @r.sort('dogs', :get => 'dog_*', :limit => [0,1], :order => 'desc alpha').should == ['taj']
  end

  it "should be able to handle array of :get using SORT" do
    @r['dog:1:name'] = 'louie'
    @r['dog:1:breed'] = 'mutt'
    @r.push_tail 'dogs', 1
    @r['dog:2:name'] = 'lucy'
    @r['dog:2:breed'] = 'poodle'
    @r.push_tail 'dogs', 2
    @r['dog:3:name'] = 'max'
    @r['dog:3:breed'] = 'hound'
    @r.push_tail 'dogs', 3
    @r['dog:4:name'] = 'taj'
    @r['dog:4:breed'] = 'terrier'
    @r.push_tail 'dogs', 4
    @r.sort('dogs', :get => ['dog:*:name', 'dog:*:breed'], :limit => [0,1]).should == ['louie', 'mutt']
    @r.sort('dogs', :get => ['dog:*:name', 'dog:*:breed'], :limit => [0,1], :order => 'desc alpha').should == ['taj', 'terrier']
  end
  # 
  it "should provide info" do
    [:last_save_time, :redis_version, :total_connections_received, :connected_clients, :total_commands_processed, :connected_slaves, :uptime_in_seconds, :used_memory, :uptime_in_days, :changes_since_last_save].each do |x|
    @r.info.keys.should include(x)
    end
  end
  # 
  it "should be able to flush the database" do
    @r['key1'] = 'keyone'
    @r['key2'] = 'keytwo'
    @r.keys('*').sort.should == ['foo', 'key1', 'key2'] #foo from before
    @r.flush_db
    @r.keys('*').should == []
  end
  # 
  it "should be able to provide the last save time" do
    savetime = @r.last_save
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
     lambda {@r.bgsave}.should_not raise_error(RedisError)
  end
  
  it "should handle multiple servers" do
    require 'dist_redis'
    @r = DistRedis.new('localhost:6379', '127.0.0.1:6379')
    @r.select_db(15) # use database 15 for testing so we dont accidentally step on you real data

    100.times do |idx|
      @r[idx] = "foo#{idx}"
    end

    100.times do |idx|
      @r[idx].should == "foo#{idx}"
    end
  end

  it "should be able to pipeline writes" do
    @r.pipelined do |pipeline|
      pipeline.push_head "list", "hello"
      pipeline.push_head "list", 42
    end
    
    @r.type?('list').should == "list"
    @r.list_length('list').should == 2
    @r.pop_head('list').should == '42'
    @r.delete('list')
  end
  
  it "should select db on connection"
  it "should re-select db on reconnection"
end
