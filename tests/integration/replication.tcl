start_server {tags {"repl"}} {
    start_server {} {
        test {First server should have role slave after SLAVEOF} {
            r -1 slaveof [srv 0 host] [srv 0 port]
            after 1000
            s -1 role
        } {slave}

        test {MASTER and SLAVE dataset should be identical after complex ops} {
            createComplexDataset r 10000
            after 500
            if {[r debug digest] ne [r -1 debug digest]} {
                set csv1 [csvdump r]
                set csv2 [csvdump {r -1}]
                set fd [open /tmp/repldump1.txt w]
                puts -nonewline $fd $csv1
                close $fd
                set fd [open /tmp/repldump2.txt w]
                puts -nonewline $fd $csv2
                close $fd
                puts "Master - Slave inconsistency"
                puts "Run diff -u against /tmp/repldump*.txt for more info"
            }
            assert_equal [r debug digest] [r -1 debug digest]
        }

        test {MASTER and SLAVE consistency with expire} {
            createComplexDataset r 50000 useexpire
            after 4000 ;# Make sure everything expired before taking the digest
            if {[r debug digest] ne [r -1 debug digest]} {
                set csv1 [csvdump r]
                set csv2 [csvdump {r -1}]
                set fd [open /tmp/repldump1.txt w]
                puts -nonewline $fd $csv1
                close $fd
                set fd [open /tmp/repldump2.txt w]
                puts -nonewline $fd $csv2
                close $fd
                puts "Master - Slave inconsistency"
                puts "Run diff -u against /tmp/repldump*.txt for more info"
            }
            assert_equal [r debug digest] [r -1 debug digest]
        }
    }
}

start_server {tags {"repl"}} {
    r set mykey foo
    
    start_server {} {
        test {Second server should have role master at first} {
            s role
        } {master}
        
        test {SLAVEOF should start with link status "down"} {
            r slaveof [srv -1 host] [srv -1 port]
            s master_link_status
        } {down}
        
        test {The role should immediately be changed to "slave"} {
            s role
        } {slave}

        wait_for_sync r
        test {Sync should have transferred keys from master} {
            r get mykey
        } {foo}
        
        test {The link status should be up} {
            s master_link_status
        } {up}
        
        test {SET on the master should immediately propagate} {
            r -1 set mykey bar
            r  0 get mykey
        } {bar}
    }
}
