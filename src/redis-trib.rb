#!/usr/bin/env ruby

def colorized(str, color)
    return str if !(ENV['TERM'] || '')["xterm"]
    color_code = {
        white: 29,
        bold: '29;1',
        black: 30,
        red: 31,
        green: 32,
        yellow: 33,
        blue: 34,
        magenta: 35,
        cyan: 36,
        gray: 37
    }[color]
    return str if !color_code
    "\033[#{color_code}m#{str}\033[0m"
end

class String

    %w(white bold black red green yellow blue magenta cyan gray).each{|color|
        color = :"#{color}"
        define_method(color){
            colorized(self, color)
        }
    }

end

COMMANDS = %w(create check info fix reshard rebalance add-node 
              del-node set-timeout call import help)

ALLOWED_OPTIONS={
    "create" => {"replicas" => true},
    "add-node" => {"slave" => false, "master-id" => true},
    "import" => {"from" => :required, "copy" => false, "replace" => false},
    "reshard" => {"from" => true, "to" => true, "slots" => true, "yes" => false, "timeout" => true, "pipeline" => true},
    "rebalance" => {"weight" => [], "auto-weights" => false, "use-empty-masters" => false, "timeout" => true, "simulate" => false, "pipeline" => true, "threshold" => true},
    "fix" => {"timeout" => 0},
}

def parse_options(cmd)
    cmd = cmd.downcase
    idx = 0
    options = {}
    args = []
    while (arg = ARGV.shift)
        if arg[0..1] == "--"
            option = arg[2..-1]

            # --verbose is a global option
            if option == "--verbose"
                options['verbose'] = true
                next
            end
            if ALLOWED_OPTIONS[cmd] == nil || 
               ALLOWED_OPTIONS[cmd][option] == nil
                next
            end
            if ALLOWED_OPTIONS[cmd][option] != false
                value = ARGV.shift
                next if !value
            else
                value = true
            end

            # If the option is set to [], it's a multiple arguments
            # option. We just queue every new value into an array.
            if ALLOWED_OPTIONS[cmd][option] == []
                options[option] = [] if !options[option]
                options[option] << value
            else
                options[option] = value
            end
        else
            next if arg[0,1] == '-'
            args << arg
        end
    end

    return options,args
end

def command_example(cmd, args, opts)
    cmd = "redis-cli --cluster #{cmd}"
    args.each{|a| 
        a = a.to_s
        a = a.inspect if a[' ']
        cmd << " #{a}"
    }
    opts.each{|opt, val|
        opt = " --cluster-#{opt.downcase}"
        if val != true
            val = val.join(' ') if val.is_a? Array
            opt << " #{val}"
        end
        cmd << opt
    }
    cmd
end

$command = ARGV.shift
$opts, $args = parse_options($command) if $command

puts "WARNING: redis-trib.rb is not longer available!".yellow
puts "You should use #{'redis-cli'.bold} instead."
puts ''
puts "All commands and features belonging to redis-trib.rb "+
     "have been moved\nto redis-cli."
puts "In order to use them you should call redis-cli with the #{'--cluster'.bold}"
puts "option followed by the subcommand name, arguments and options."
puts ''
puts "Use the following syntax:"
puts "redis-cli --cluster SUBCOMMAND [ARGUMENTS] [OPTIONS]".bold
puts ''
puts "Example:"
if $command
    example = command_example $command, $args, $opts
else
    example = "redis-cli --cluster info 127.0.0.1:7000"
end
puts example.bold
puts ''
puts "To get help about all subcommands, type:"
puts "redis-cli --cluster help".bold
puts ''
exit 1
