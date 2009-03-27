require 'zlib'

class HashRing

  POINTS_PER_SERVER = 160 # this is the default in libmemcached

  attr_reader :ring, :sorted_keys, :replicas, :nodes

  # nodes is a list of objects that have a proper to_s representation.
  # replicas indicates how many virtual points should be used pr. node,
  # replicas are required to improve the distribution.
  def initialize(nodes=[], replicas=POINTS_PER_SERVER)
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
      key = Zlib.crc32("#{node}:#{i}")
      @ring[key] = node
      @sorted_keys << key
    end
    @sorted_keys.sort!
  end
  
  def remove_node(node)
    @replicas.times do |i|
      key = Zlib.crc32("#{node}:#{count}")
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
    crc = Zlib.crc32(key)
    idx = HashRing.binary_search(@sorted_keys, crc)
    return [@ring[@sorted_keys[idx]], idx]
  end
  
  def iter_nodes(key)
    return [nil,nil] if @ring.size == 0
    node, pos = get_node_pos(key)
    @sorted_keys[pos..-1].each do |k|
      yield @ring[k]
    end  
  end
  
  class << self

    # gem install RubyInline to use this code
    # Native extension to perform the binary search within the hashring.
    # There's a pure ruby version below so this is purely optional
    # for performance.  In testing 20k gets and sets, the native
    # binary search shaved about 12% off the runtime (9sec -> 8sec).
    begin
      require 'inline'
      inline do |builder|
        builder.c <<-EOM
        int binary_search(VALUE ary, unsigned int r) {
            int upper = RARRAY_LEN(ary) - 1;
            int lower = 0;
            int idx = 0;

            while (lower <= upper) {
                idx = (lower + upper) / 2;

                VALUE continuumValue = RARRAY_PTR(ary)[idx];
                unsigned int l = NUM2UINT(continuumValue);
                if (l == r) {
                    return idx;
                }
                else if (l > r) {
                    upper = idx - 1;
                }
                else {
                    lower = idx + 1;
                }
            }
            return upper;
        }
        EOM
      end
    rescue Exception => e
      # Find the closest index in HashRing with value <= the given value
      def binary_search(ary, value, &block)
        upper = ary.size - 1
        lower = 0
        idx = 0

        while(lower <= upper) do
          idx = (lower + upper) / 2
          comp = ary[idx] <=> value

          if comp == 0
            return idx
          elsif comp > 0
            upper = idx - 1
          else
            lower = idx + 1
          end
        end
        return upper
      end

    end
  end

end

# ring = HashRing.new ['server1', 'server2', 'server3']
# p ring
# #
# p ring.get_node "kjhjkjlkjlkkh"
# 