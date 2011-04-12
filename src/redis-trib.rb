#!/usr/bin/env ruby

require 'rubygems'
require 'redis'

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

    def r
        @r
    end
end

class RedisTrib
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
        }
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
