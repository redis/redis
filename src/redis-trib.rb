#!/usr/bin/env ruby

require 'rubygems'
require 'redis'

ClusterHashSlots = 4096

def xputs(s)
    printf s
    STDOUT.flush
end

class ClusterNode
    def initialize(addr)
        s = addr.split(":")
        if s.length != 2
            puts "Invalid node name #{addr}"
            exit 1
        end
        @r = nil
        @host = s[0]
        @port = s[1]
        @slots = {}
        @dirty = false
    end

    def to_s
        "#{@host}:#{@port}"
    end

    def connect(o={})
        xputs "Connecting to node #{self}: "
        begin
            @r = Redis.new(:host => @ost, :port => @port)
            @r.ping
        rescue
            puts "ERROR"
            puts "Sorry, can't connect to node #{self}"
            exit 1 if o[:abort]
            @r = nil
        end
        puts "OK"
    end

    def assert_cluster
        info = @r.info
        if !info["cluster_enabled"] || info["cluster_enabled"].to_i == 0
            puts "Error: Node #{self} is not configured as a cluster node."
            exit 1
        end
    end

    def assert_empty
        if !(@r.cluster("info").split("\r\n").index("cluster_known_nodes:1")) ||
            (@r.info['db0'])
            puts "Error: Node #{self} is not empty. Either the node already knows other nodes (check with nodes-info) or contains some key in database 0."
            exit 1
        end
    end

    def add_slots(slots)
        slots.each{|s|
            @slots[s] = :new
        }
        @dirty = true
    end

    def flush_node_config
        return if !@dirty
        new = []
        @slots.each{|s,val|
            if val == :new
                new << s
                @slots[s] = true
            end
        }
        @r.cluster("addslots",*new)
        @dirty = false
    end

    def info_string
        # We want to display the hash slots assigned to this node
        # as ranges, like in: "1-5,8,9,20-35,30"
        #
        # Note: this could be easily written without side effects,
        # we use 'slots' just to split the computation into steps.
        
        # First step: we want an increasing array of integers
        # for instance: [1,2,3,4,5,8,9,20,21,22,23,24,25,30]
        slots = @slots.keys.sort

        # As we want to aggregate adiacent slots we convert all the
        # slot integers into ranges (with just one element)
        # So we have something like [1..1,2..2, ... and so forth.
        slots.map!{|x| x..x}

        # Finally we group ranges with adiacent elements.
        slots = slots.reduce([]) {|a,b|
            if !a.empty? && b.first == (a[-1].last)+1
                a[0..-2] + [(a[-1].first)..(b.last)]
            else
                a + [b]
            end
        }

        # Now our task is easy, we just convert ranges with just one
        # element into a number, and a real range into a start-end format.
        # Finally we join the array using the comma as separator.
        slots = slots.map{|x|
            x.count == 1 ? x.first.to_s : "#{x.first}-#{x.last}"
        }.join(",")

        "#{self.to_s.ljust(25)} slots:#{slots}"
    end

    def info
        {
            :host => @host,
            :port => @port,
            :slots => @slots,
            :dirty => @dirty
        }
    end
    
    def is_dirty?
        @dirty
    end

    def r
        @r
    end
end

class RedisTrib
    def initialize
        @nodes = []
    end

    def check_arity(req_args, num_args)
        if ((req_args > 0 and num_args != req_args) ||
           (req_args < 0 and num_args < req_args.abs))
           puts "Wrong number of arguments for specified sub command"
           exit 1
        end
    end

    def add_node(node)
        @nodes << node
    end

    def create_cluster
        puts "Creating cluster"
        ARGV[1..-1].each{|n|
            node = ClusterNode.new(n)
            node.connect(:abort => true)
            node.assert_cluster
            node.assert_empty
            add_node(node)
        }
        puts "Performing hash slots allocation on #{@nodes.length} nodes..."
        alloc_slots
        show_nodes
        yes_or_die "Can I set the above configuration?"
        flush_nodes_config
        puts "** Nodes configuration updated"
        puts "** Sending CLUSTER MEET messages to join the cluster"
        join_cluster
        check_cluster
    end

    def check_cluster
        puts "Performing Cluster Check (node #{ARGV[1]})"
        node = ClusterNode.new(ARGV[1])
        node.connect(:abort => true)
        node.assert_cluster
        node.add_slots(10..15)
        node.add_slots(30..30)
        node.add_slots(5..5)
        add_node(node)
        show_nodes
    end

    def alloc_slots
        slots_per_node = ClusterHashSlots/@nodes.length
        i = 0
        @nodes.each{|n|
            first = i*slots_per_node
            last = first+slots_per_node-1
            last = ClusterHashSlots-1 if i == @nodes.length-1
            n.add_slots first..last
            i += 1
        }
    end

    def flush_nodes_config
        @nodes.each{|n|
            n.flush_node_config
        }
    end

    def show_nodes
        @nodes.each{|n|
            puts n.info_string
        }
    end

    def join_cluster
        # We use a brute force approach to make sure the node will meet
        # each other, that is, sending CLUSTER MEET messages to all the nodes
        # about the very same node.
        # Thanks to gossip this information should propagate across all the
        # cluster in a matter of seconds.
        first = false
        @nodes.each{|n|
            if !first then first = n.info; next; end # Skip the first node
            n.r.cluster("meet",first[:host],first[:port])
        }
    end

    def yes_or_die(msg)
        print "#{msg} (type 'yes' to accept): "
        STDOUT.flush
        if !(STDIN.gets.chomp.downcase == "yes")
            puts "Aborting..."
            exit 1
        end
    end
end

COMMANDS={
    "create" => ["create_cluster", -2, "host1:port host2:port ... hostN:port"],
    "check" =>  ["check_cluster", 2, "host:port"]
}

# Sanity check
if ARGV.length == 0
    puts "Usage: redis-trib <command> <arguments ...>"
    puts
    COMMANDS.each{|k,v|
        puts "  #{k.ljust(20)} #{v[2]}"
    }
    puts
    exit 1
end

rt = RedisTrib.new
cmd_spec = COMMANDS[ARGV[0].downcase]
if !cmd_spec
    puts "Unknown redis-trib subcommand '#{ARGV[0]}'"
    exit 1
end
rt.check_arity(cmd_spec[1],ARGV.length)

# Dispatch
rt.send(cmd_spec[0])
