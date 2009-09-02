require 'redis'
require 'hash_ring'
class DistRedis
  attr_reader :ring
  def initialize(opts={})
    hosts = []

    db = opts[:db] || nil
    timeout = opts[:timeout] || nil

    raise Error, "No hosts given" unless opts[:hosts]

    opts[:hosts].each do |h|
      host, port = h.split(':')
      hosts << Redis.new(:host => host, :port => port, :db => db, :timeout => timeout)
    end

    @ring = HashRing.new hosts
  end

  def node_for_key(key)
    key = $1 if key =~ /\{(.*)?\}/
    @ring.get_node(key)
  end

  def add_server(server)
    server, port = server.split(':')
    @ring.add_node Redis.new(:host => server, :port => port)
  end

  def method_missing(sym, *args, &blk)
    if redis = node_for_key(args.first.to_s)
      redis.send sym, *args, &blk
    else
      super
    end
  end

  def keys(glob)
    @ring.nodes.map do |red|
      red.keys(glob)
    end
  end

  def save
    on_each_node :save
  end

  def bgsave
    on_each_node :bgsave
  end

  def quit
    on_each_node :quit
  end

  def flush_all
    on_each_node :flush_all
  end
  alias_method :flushall, :flush_all

  def flush_db
    on_each_node :flush_db
  end
  alias_method :flushdb, :flush_db

  def delete_cloud!
    @ring.nodes.each do |red|
      red.keys("*").each do |key|
        red.delete key
      end
    end
  end

  def on_each_node(command, *args)
    @ring.nodes.each do |red|
      red.send(command, *args)
    end
  end

end


if __FILE__ == $0

r = DistRedis.new 'localhost:6379', 'localhost:6380', 'localhost:6381', 'localhost:6382'
  r['urmom'] = 'urmom'
  r['urdad'] = 'urdad'
  r['urmom1'] = 'urmom1'
  r['urdad1'] = 'urdad1'
  r['urmom2'] = 'urmom2'
  r['urdad2'] = 'urdad2'
  r['urmom3'] = 'urmom3'
  r['urdad3'] = 'urdad3'
  p r['urmom']
  p r['urdad']
  p r['urmom1']
  p r['urdad1']
  p r['urmom2']
  p r['urdad2']
  p r['urmom3']
  p r['urdad3']

  r.push_tail 'listor', 'foo1'
  r.push_tail 'listor', 'foo2'
  r.push_tail 'listor', 'foo3'
  r.push_tail 'listor', 'foo4'
  r.push_tail 'listor', 'foo5'

  p r.pop_tail('listor')
  p r.pop_tail('listor')
  p r.pop_tail('listor')
  p r.pop_tail('listor')
  p r.pop_tail('listor')

  puts "key distribution:"

  r.ring.nodes.each do |red|
    p [red.port, red.keys("*")]
  end
  r.delete_cloud!
  p r.keys('*')

end
