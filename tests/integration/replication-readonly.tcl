start_server {tags {"repl"}} {
    start_server {} {
        test {First server should have role slave after SLAVEOF and readonly} {
            r -1 config set readonly yes
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
    }
}
