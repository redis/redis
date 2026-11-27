# riscv-siphash: runtime coverage for the RISC-V siphash cores.
#
# Word-level hashes are bit-identical between the portable and the Zbb core
# (verified by the built-in self-test in src/siphash.c), so all commands below
# run against whichever core the platform selected at startup.
# SIPHASH_DISABLE_ZBB forces the portable core (fallback path).

start_server {tags {"siphash"}} {
    test {siphash: int and string key lookup agree on stored values} {
        r flushdb
        for {set i 0} {$i < 100} {incr i} {
            r set key:$i $i
        }
        r mset a 1 b 2 c 3 d 4
        assert_equal [r mget a b c d] {1 2 3 4}
        assert_equal [r get key:50] 50
        assert_equal [r dbsize] 104
    }

    test {siphash: keys stay case-sensitive (siphash_nocase is internal-only)} {
        r set SOMEKEY 1
        assert_equal [r get SOMEKEY] 1
        assert_equal [r get somekey] {}
        r del SOMEKEY
        assert_equal [r exists somekey] 0
    }

    test {siphash: scan sees every key (iteration over hash table)} {
        r flushdb
        for {set i 1} {$i <= 2000} {incr i} {
            r set "k$i" $i
        }
        set seen 0
        set cursor 0
        while 1 {
            set res [r scan $cursor COUNT 500]
            set cursor [lindex $res 0]
            set keys [lindex $res 1]
            set seen [expr {$seen + [llength $keys]}]
            if {$cursor == 0} break
        }
        assert_equal $seen 2000
    }
}