require 'digest/md5'
class HashRing
  attr_reader :ring, :sorted_keys, :replicas, :nodes
  # nodes is a list of objects that have a proper to_s representation.
  # replicas indicates how many virtual points should be used pr. node,
  # replicas are required to improve the distribution.
  def initialize(nodes=[], replicas=3)
    @replicas = replicas
    @ring = {}
    @nodes = []
    @sorted_keys = []
    nodes.each do |node|
      add_node(node)
    end
  end
  
  # Adds a `node` to the hash ring (including a number of replicas).
  def add_node(node)
    @nodes << node
    @replicas.times do |i|
      key = gen_key("#{node}:#{i}")
      @ring[key] = node
      @sorted_keys << key
    end
    @sorted_keys.sort!
  end
  
  def remove_node(node)
    @replicas.times do |i|
      key = gen_key("#{node}:#{count}")
      @ring.delete(key)
      @sorted_keys.reject! {|k| k == key}
    end
  end
  
  # get the node in the hash ring for this key
  def get_node(key)
    get_node_pos(key)[0]
  end
  
  def get_node_pos(key)
    return [nil,nil] if @ring.size == 0
    key = gen_key(key)
    nodes = @sorted_keys
    nodes.size.times do |i|
      node = nodes[i]
      if key <= node
        return [@ring[node], i]
      end
    end
    [@ring[nodes[0]], 0]
  end
  
  def iter_nodes(key)
    return [nil,nil] if @ring.size == 0
    node, pos = get_node_pos(key)
    @sorted_keys[pos..-1].each do |k|
      yield @ring[k]
    end  
  end
  
  def gen_key(key)
    key = Digest::MD5.hexdigest(key)
    ((key[3] << 24) | (key[2] << 16) | (key[1] << 8) | key[0])
  end
  
end

# ring = HashRing.new ['server1', 'server2', 'server3']
# p ring
# #
# p ring.get_node "kjhjkjlkjlkkh"
# 