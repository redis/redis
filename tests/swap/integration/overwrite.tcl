start_server {} {
    # TO STRING: BITOP, SET, SETEX, PSETEX, MSET
    # TO SET: SINTERSTORE, SUNIONSTORE, SDIFFSTORE
    # TO ZSET: GEOSEARCHSTORE, ZUNIONSTORE, ZDIFFSTORE, ZINTERSTORE, ZRANGESTORE
    # TO LIST: SORT
    foreach {type} {string hash set zset list} {
        createComplexDataset r 2000

        test "\[BITOP\] OVERWRITE TO STRING - $type" {
            r del t1 t2
            r mset t1 abc t2 123
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r BITOP OR $otherkey t1 t2
                }
            }
            r save
        }

        test "\[SET\] OVERWRITE TO STRING - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r SET $otherkey foo
                }
            }
            r save
        }

        test "\[SETEX\] OVERWRITE TO STRING - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r SETEX $otherkey 120 foo
                }
            }
            r save
        }

        test "\[PSETEX\] OVERWRITE TO STRING - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r PSETEX $otherkey 120000 foo
                }
            }
            r save
        }

        test "\[MSET\] OVERWRITE TO STRING - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r MSET $otherkey foo
                }
            }
            r save
        }

        r del s1 s2
        r sadd s1 a b c
        r sadd s2 b c d
        test "\[SINTERSTORE\] OVERWRITE TO SET - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r SINTERSTORE $otherkey s1 s2
                }
            }
            r save
        }

        test "\[SUNIONSTORE\] OVERWRITE TO SET - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r SUNIONSTORE $otherkey s1 s2
                }
            }
            r save
        }

        test "\[SDIFFSTORE\] OVERWRITE TO SET - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r SDIFFSTORE $otherkey s1 s2
                }
            }
            r save
        }

        r del geo
        r GEOADD geo 1.1 1.2 "p1" 2.1 2.2 "p2"
        r GEOADD geo 3.1 3.2 "p3" 4.1 4.2 "p4"
        test "\[GEOSEARCHSTORE\] OVERWRITE TO ZSET - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r GEOSEARCHSTORE $otherkey geo FROMLONLAT 2 2 BYRADIUS 100 km ASC
                }
            }
            r save
        }

        r del z1 z2
        r zadd z1 0.1 f1 0.2 f2
        r zadd z2 0.3 f2 0.4 f3
        test "\[ZUNIONSTORE\] OVERWRITE TO ZSET - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r ZUNIONSTORE $otherkey 2 z1 z2
                }
            }
            r save
        }

        test "\[ZDIFFSTORE\] OVERWRITE TO ZSET - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r ZDIFFSTORE $otherkey 2 z1 z2
                }
            }
            r save
        }

        test "\[ZINTERSTORE\] OVERWRITE TO ZSET - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r ZINTERSTORE $otherkey 2 z1 z2
                }
            }
            r save
        }

        test "\[ZRANGESTORE\] OVERWRITE TO ZSET - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r ZRANGESTORE $otherkey z1 0 -1
                }
            }
            r save
        }

        r del s1
        r sadd s1 q w e r t y u i o
        test "\[SORT\] OVERWRITE TO LIST - $type" {
            for {set i 0} {$i < 10} {incr i} {
                set otherkey [findKeyWithType r $type]
                if {$otherkey ne {}} {
                    r SORT s1 ALPHA STORE $otherkey
                }
            }
            r save
        }
    }
}
