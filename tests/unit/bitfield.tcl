start_server {tags {"bitops"}} {
    test {BITFIELD overflow check fuzzing} {
        for {set j 0} {$j < 1000} {incr j} {
            set bits [expr {[randomInt 64]+1}]
            set sign [randomInt 2]
            set range [expr {2**$bits}]
            if {$bits == 64} {set sign 1} ; # u64 is not supported by BITFIELD.
            if {$sign} {
                set min [expr {-($range/2)}]
                set type "i$bits"
            } else {
                set min 0
                set type "u$bits"
            }
            set max [expr {$min+$range-1}]
            # Compare Tcl vs Redis
            set range2 [expr {$range*2}]
            set value [expr {($min*2)+[randomInt $range2]}]
            set increment [expr {($min*2)+[randomInt $range2]}]
            if {$value > 9223372036854775807} {
                set value 9223372036854775807
            }
            if {$value < -9223372036854775808} {
                set value -9223372036854775808
            }
            if {$increment > 9223372036854775807} {
                set increment 9223372036854775807
            }
            if {$increment < -9223372036854775808} {
                set increment -9223372036854775808
            }

            set overflow 0
            if {$value > $max || $value < $min} {set overflow 1}
            if {($value + $increment) > $max} {set overflow 1}
            if {($value + $increment) < $min} {set overflow 1}

            r del bits
            set res1 [r bitfield bits overflow fail set $type 0 $value]
            set res2 [r bitfield bits overflow fail incrby $type 0 $increment]

            if {$overflow && [lindex $res1 0] ne {} &&
                             [lindex $res2 0] ne {}} {
                fail "OW not detected where needed: $type $value+$increment"
            }
            if {!$overflow && ([lindex $res1 0] eq {} ||
                               [lindex $res2 0] eq {})} {
                fail "OW detected where NOT needed: $type $value+$increment"
            }
        }
    }
}
