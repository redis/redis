#!/usr/bin/env ruby

require 'rubygems'
require 'redis'

class RedisTrib
    def xputs(s)
        printf s
        STDOUT.flush
    end

    def check_arity(req_args, num_args)
        if ((req_args > 0 and num_args != req_args) ||
           (req_args < 0 and num_args < req_args.abs))
           puts "Wrong number of arguments for specified sub command"
           exit 1
        end
    end

    def parse_node(node)
        s = node.split(":")
        if s.length != 2
            puts "Invalid node name #{node}"
            exit 1
        end
        return {:host => s[0], :port => s[1].to_i}
    end

    def connect_to_node(naddr)
        xputs "Connecting to node #{naddr[:host]}:#{naddr[:port]}: "
        begin
            r = Redis.new(:host => naddr[:host], :port => naddr[:port])
            r.ping
        rescue
            puts "ERROR"
            puts "Sorry, can't connect to node #{naddr[:host]}:#{naddr[:port]}"
            exit 1
        end
        puts "OK"
    end

    def create_cluster
        puts "Creating cluster"
        ARGV[1..-1].each{|node|
            naddr = parse_node(node)
            r = connect_to_node(naddr)
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
