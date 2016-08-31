require 'rubygems'
require 'redis'

$runs = []; # Remember the error rate of each run for average purposes.
$o = {};    # Options set parsing arguments

def testit(filename)
    r = Redis.new
    r.config("SET","maxmemory","2000000")
    if $o[:ttl]
        r.config("SET","maxmemory-policy","volatile-ttl")
    else
        r.config("SET","maxmemory-policy","allkeys-lru")
    end
    r.config("SET","maxmemory-samples",5)
    r.config("RESETSTAT")
    r.flushall

    html = ""
    html << <<EOF
    <html>
    <body>
    <style>
    .box {
        width:5px;
        height:5px;
        float:left;
        margin: 1px;
    }

    .old {
        border: 1px black solid;
    }

    .new {
        border: 1px green solid;
    }

    .otherdb {
        border: 1px red solid;
    }

    .ex {
        background-color: #666;
    }
    </style>
    <pre>
EOF

    # Fill the DB up to the first eviction.
    oldsize = r.dbsize
    id = 0
    while true
        id += 1
        begin
            r.set(id,"foo")
        rescue
            break
        end
        newsize = r.dbsize
        break if newsize == oldsize # A key was evicted? Stop.
        oldsize = newsize
    end

    inserted = r.dbsize
    first_set_max_id = id
    html << "#{r.dbsize} keys inserted.\n"

    # Access keys sequentially, so that in theory the first part will be expired
    # and the latter part will not, according to perfect LRU.

    if $o[:ttl]
        STDERR.puts "Set increasing expire value"
        (1..first_set_max_id).each{|id|
            r.expire(id,1000+id)
            STDERR.print(".") if (id % 150) == 0
        }
    else
        STDERR.puts "Access keys sequentially"
        (1..first_set_max_id).each{|id|
            r.get(id)
            sleep 0.001
            STDERR.print(".") if (id % 150) == 0
        }
    end
    STDERR.puts

    # Insert more 50% keys. We expect that the new keys will rarely be expired
    # since their last access time is recent compared to the others.
    #
    # Note that we insert the first 100 keys of the new set into DB1 instead
    # of DB0, so that we can try how cross-DB eviction works.
    half = inserted/2
    html << "Insert enough keys to evict half the keys we inserted.\n"
    add = 0

    otherdb_start_idx = id+1
    otherdb_end_idx = id+100
    while true
        add += 1
        id += 1
        if id >= otherdb_start_idx && id <= otherdb_end_idx
            r.select(1)
            r.set(id,"foo")
            r.select(0)
        else
            r.set(id,"foo")
        end
        break if r.info['evicted_keys'].to_i >= half
    end

    html << "#{add} additional keys added.\n"
    html << "#{r.dbsize} keys in DB.\n"

    # Check if evicted keys respect LRU
    # We consider errors from 1 to N progressively more serious as they violate
    # more the access pattern.

    errors = 0
    e = 1
    error_per_key = 100000.0/first_set_max_id
    half_set_size = first_set_max_id/2
    maxerr = 0
    (1..(first_set_max_id/2)).each{|id|
        if id >= otherdb_start_idx && id <= otherdb_end_idx
            r.select(1)
            exists = r.exists(id)
            r.select(0)
        else
            exists = r.exists(id)
        end
        if id < first_set_max_id/2
            thiserr = error_per_key * ((half_set_size-id).to_f/half_set_size)
            maxerr += thiserr
            errors += thiserr if exists
        elsif id >= first_set_max_id/2
            thiserr = error_per_key * ((id-half_set_size).to_f/half_set_size)
            maxerr += thiserr
            errors += thiserr if !exists
        end
    }
    errors = errors*100/maxerr

    STDERR.puts "Test finished with #{errors}% error! Generating HTML on stdout."

    html << "#{errors}% error!\n"
    html << "</pre>"
    $runs << errors

    # Generate the graphical representation
    (1..id).each{|id|
        # Mark first set and added items in a different way.
        c = "box"
        if id >= otherdb_start_idx && id <= otherdb_end_idx
            c << " otherdb"
        elsif id <= first_set_max_id
            c << " old"
        else
            c << " new"
        end

        # Add class if exists
        if id >= otherdb_start_idx && id <= otherdb_end_idx
            r.select(1)
            exists = r.exists(id)
            r.select(0)
        else
            exists = r.exists(id)
        end

        c << " ex" if exists
        html << "<div title=\"#{id}\" class=\"#{c}\"></div>"
    }

    # Close HTML page

    html << <<EOF
    </body>
    </html>
EOF

    f = File.open(filename,"w")
    f.write(html)
    f.close
end

def print_avg
    avg = ($runs.reduce {|a,b| a+b}) / $runs.length
    puts "#{$runs.length} runs, AVG is #{avg}"
end

if ARGV.length < 1
    STDERR.puts "Usage: ruby test-lru.rb <html-output-filename> [--runs <count>] [--ttl]"
    STDERR.puts "Options:"
    STDERR.puts "  --runs <count>    Execute the test <count> times."
    STDERR.puts "  --ttl             Set keys with increasing TTL values"
    STDERR.puts "                    (starting from 1000 seconds) in order to"
    STDERR.puts "                    test the volatile-lru policy."
    exit 1
end

filename = ARGV[0]
$o[:numruns] = 1

# Options parsing
i = 1
while i < ARGV.length
    if ARGV[i] == '--runs'
        $o[:numruns] = ARGV[i+1].to_i
        i+= 1
    elsif ARGV[i] == '--ttl'
        $o[:ttl] = true
    else
        STDERR.puts "Unknown option #{ARGV[i]}"
        exit 1
    end
    i+= 1
end

$o[:numruns].times {
    testit(filename)
    print_avg if $o[:numruns] != 1
}
