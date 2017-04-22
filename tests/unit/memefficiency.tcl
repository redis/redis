proc test_memory_efficiency {range} {
    r flushall
    set rd [redis_deferring_client]
    set base_mem [s used_memory]
    set written 0
    for {set j 0} {$j < 10000} {incr j} {
        set key key:$j
        set val [string repeat A [expr {int(rand()*$range)}]]
        $rd set $key $val
        incr written [string length $key]
        incr written [string length $val]
        incr written 2 ;# A separator is the minimum to store key-value data.
    }
    for {set j 0} {$j < 10000} {incr j} {
        $rd read ; # Discard replies
    }

    set current_mem [s used_memory]
    set used [expr {$current_mem-$base_mem}]
    set efficiency [expr {double($written)/$used}]
    return $efficiency
}

start_server {tags {"memefficiency"}} {
    foreach {size_range expected_min_efficiency} {
        32    0.15
        64    0.25
        128   0.35
        1024  0.75
        16384 0.82
    } {
        test "Memory efficiency with values in range $size_range" {
            set efficiency [test_memory_efficiency $size_range]
            assert {$efficiency >= $expected_min_efficiency}
        }
    }
}

if 0 {
    start_server {tags {"defrag"}} {
        if {[string match {*jemalloc*} [s mem_allocator]]} {
            test "Active defrag" {
                r config set activedefrag no
                r config set active-defrag-threshold-lower 5
                r config set active-defrag-ignore-bytes 2mb
                r config set maxmemory 100mb
                r config set maxmemory-policy allkeys-lru
                r debug populate 700000 asdf 150
                r debug populate 170000 asdf 300
                set frag [s mem_fragmentation_ratio]
                assert {$frag >= 1.7}
                r config set activedefrag yes
                after 1500 ;# active defrag tests the status once a second.
                set hits [s active_defrag_hits]

                # wait for the active defrag to stop working
                set tries 0
                while { True } {
                    incr tries
                    after 500
                    set prev_hits $hits
                    set hits [s active_defrag_hits]
                    if {$hits == $prev_hits} {
                        break
                    }
                    assert {$tries < 100}
                }

                # TODO: we need to expose more accurate fragmentation info
                # i.e. the allocator used and active pages
                # instead we currently look at RSS so we need to ask for purge
                r memory purge

                # Test the the fragmentation is lower and that the defragger
                # stopped working
                set frag [s mem_fragmentation_ratio]
                assert {$frag < 1.55}
                set misses [s active_defrag_misses]
                after 500
                set misses2 [s active_defrag_misses]
                assert {$misses2 == $misses}
            }
        }
    }
}
