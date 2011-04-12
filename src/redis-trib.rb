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
            puts "Invalid node name #{node}"
            exit 1
        end
        @host = s[0]
        @port = s[1]
        @slots = {}
        @dirty = false
    end

    def to_s
        "#{@host}:#{@port}"
    end

    def connect
        xputs "Connecting to node #{self}: "
        begin
            @r = Redis.new(:host => @ost, :port => @port)
            @r.ping
        rescue
            puts "ERROR"
            puts "Sorry, can't connect to node #{self}"
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

    def info
        slots = @slots.map{|k,v| k}.reduce{|a,b|
            a = [(a..a)] if !a.is_a?(Array)
            if b == (a[-1].last)+1
                a[-1] = (a[-1].first)..b
                a
            else
                a << (b..b)
            end
        }.map{|x|
            (x.first == x.last) ? x.first.to_s : "#{x.first}-#{x.last}"
        }.join(",")
        "#{self.to_s.ljust(25)} slots:#{slots}"
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

    def create_cluster
        puts "Creating cluster"
        ARGV[1..-1].each{|n|
            node = ClusterNode.new(n)
            node.connect
            node.assert_cluster
            node.assert_empty
            @nodes << node
        }
        puts "Performing hash slots allocation on #{@nodes.length} nodes..."
        alloc_slots
        show_nodes
        yes_or_die "Can I set the above configuration?"
        flush_nodes_config
        puts "** Nodes configuration updated"
        puts "Sending CLUSTER MEET messages to join the cluster"
        join_cluster
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
            puts n.info
        }
    end

    def join_cluster
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
    "create-cluster" => ["create_cluster", -2]
}

# Sanity check
if ARGV.length == 0
    puts "Usage: redis-trib <command> <arguments ...>"
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
