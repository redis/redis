start_server {
    tags {"stream"}
} {
    test {XHASH CREATE fails on keyspace miss} {
        r DEL stash
        catch {r XHASH CREATE stash} err
        set err
    } {*MKSTREAM*}

    test {XHASH CREATE with MKSTREAM on keyspace miss creates an empty stream} {
        r DEL stash
        set reply [r XHASH CREATE stash MKSTREAM]
        assert_equal $reply "0-0"
        set reply [r TYPE stash]
        assert_equal $reply "stream"
        set reply [r XLEN stash]
        assert_equal $reply "0"
        set reply [r XHLEN stash]
        assert_equal $reply "0"
    }

    test {XHASH CREATE on an empty stream succeeds} {
        r DEL stash
        set id [r XADD stash * id 1 temp 1]
        r XDEL stash $id
        set reply [r XHASH CREATE stash]
        assert_equal $reply $id
        set reply [r XHLEN stash]
        assert_equal $reply "0"
    }

    test {XHASH CREATE on an never-used stream returns zero and an empty hash} {
        r DEL stash
        r XGROUP CREATE stash cg1 $ MKSTREAM
        set reply [r XHASH CREATE stash]
        assert_equal $reply "0-0"
        set reply [r XHLEN stash]
        assert_equal $reply "0"
    }

    test {XHASH CREATE on an existing stream succeeds with the last ID and an empty hash} {
        r DEL stash
        set id [r XADD stash * id 1 temp 1]
        set reply [r XHASH CREATE stash]
        assert_equal $reply $id
        set reply [r XHLEN stash]
        assert_equal $reply "0"
    }

    test {XHASH DESTROY returns nil on keyspace miss} {
        r DEL stash
        set reply [r XHASH DESTROY stash]
        assert_equal $reply {}
    }

    test {XHASH DESTROY returns 0 when the stream hash doesn't exist} {
        r DEL stash
        r XGROUP CREATE stash cg1 $ MKSTREAM
        set reply [r XHASH DESTROY stash]
        assert_equal $reply 0
    }

    test {XHASH DESTROY returns 1 when the stream hash exists} {
        r DEL stash
        r XHASH CREATE stash MKSTREAM
        set reply [r XHASH DESTROY stash]
        assert_equal $reply 1
    }

    test {XHLEN returns nil for keyspace miss} {
        r DEL stash
        set reply [r XHLEN stash]
        assert_equal $reply {}
    }

    test {XHLEN returns nil when the stream hash doesn't exist} {
        r DEL stash
        r XGROUP CREATE stash cg1 $ MKSTREAM
        set reply [r XHLEN stash]
        assert_equal $reply {}

        r XHASH CREATE stash
        r XHASH DESTROY stash
        set reply [r XHLEN stash]
        assert_equal $reply {}
    }

    test {A stream without a hash has no hash} {
        r DEL stash
        r XHASH CREATE stash MKSTREAM
        r XHASH DESTROY stash
        r XADD stash * id 1 temp 1
        set reply [r XHLEN stash]
        assert_equal $reply {}
    }

    test {XADD creates a new hash entry is added for a new primary value} {
        r DEL stash
        r XHASH CREATE stash MKSTREAM
        set id [r XADD stash * id 1 temp 10]
        set reply [r XHLEN stash]
        assert_equal $reply 1
        set reply [r XHGET stash 1]
        assert_equal [lindex $reply 0 1] $id
        assert_equal [lindex $reply 0 3 3] 10

        set id [r XADD stash * id 2 temp 20]
        set reply [r XHLEN stash]
        assert_equal $reply 2
        set reply [r XHGET stash 2]
        assert_equal [lindex $reply 0 1] $id
        assert_equal [lindex $reply 0 3 3] 20
    }

    test {XADD updates an existing stream hash entry} {
        r DEL stash
        r XHASH CREATE stash MKSTREAM
        r XADD stash * id 1 temp 10
        set id [r XADD stash * id 1 temp 11]
        set reply [r XHLEN stash]
        assert_equal $reply 1
        set reply [r XHGET stash 1]
        assert_equal [lindex $reply 0 1] $id
        assert_equal [lindex $reply 0 3 3] 11
    }

    test {XHGET returns nil for a non-existing stream} {
        r DEL stash
        set reply [r XHGET stash foo]
        assert_equal $reply {}
    }

    test {XHGET returns nil for a stream without a hash} {
        r DEL stash
        r XADD stash * id 1 temp 10
        set reply [r XHGET stash 1]
        assert_equal $reply {}
    }

    test {XHGET returns nil for non-existing primary values} {
        r DEL stash
        r XHASH CREATE stash MKSTREAM
        r XADD stash * id 1 temp 10
        set reply [r XHGET stash foo]
        assert_equal [llength $reply] 1
        assert_equal [lindex $reply 0 1] {}
        assert_equal [lindex $reply 0 3] {}
    }

    test {XHGET returns correctly for existing primary values} {
        r DEL stash
        r XHASH CREATE stash MKSTREAM
        set id [r XADD stash * id 1 temp 10]
        set reply [r XHGET stash 1]
        assert_equal [llength $reply] 1
        assert_equal [lindex $reply 0 1] $id
        assert_equal [lindex $reply 0 3 1] 1
        assert_equal [lindex $reply 0 3 3] 10
    }

    test {XHGET variadic form appears to work} {
        r DEL stash
        r XHASH CREATE stash MKSTREAM
        r XADD stash * id 1 temp 10
        r XADD stash * id 2 temp 20
        r XADD stash * id 1 temp 11
        set reply [r XHGET stash 1 foo 2]
        assert_equal [llength $reply] 3
        assert_equal [lindex $reply 0 3 3] 11
        assert_equal [lindex $reply 1 3] {}
        assert_equal [lindex $reply 2 3 3] 20
    }

    test {XHGETID variadic form appears to work} {
        r DEL stash
        r XHASH CREATE stash MKSTREAM
        r XADD stash * id 1 temp 10
        set id2 [r XADD stash * id 2 temp 20]
        set id1 [r XADD stash * id 1 temp 11]
        set reply [r XHGETID stash 1 foo 2]
        assert_equal [llength $reply] 3
        assert_equal [lindex $reply 0] $id1
        assert_equal [lindex $reply 1] {}
        assert_equal [lindex $reply 2] $id2
    }

    test {A hash tombstone isn't set if the non-latest primary is XDELed} {
        r DEL stash
        r XHASH CREATE stash MKSTREAM
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 1 temp 11]
        r XDEL stash $id1
        set reply [r XHGET stash 1]
        assert_equal [llength $reply] 1
        assert_equal [lindex $reply 0 1] $id2
        assert_equal [lindex $reply 0 3 3] 11
    }

    test {A hash tombstone is set if the latest primary is XDELed} {
        r DEL stash
        r XHASH CREATE stash MKSTREAM
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 1 temp 11]
        r XDEL stash $id2
        set reply [r XHGET stash 1]
        assert_equal [llength $reply] 1
        assert_equal [lindex $reply 0 1] "0-0"
        assert_equal [lindex $reply 0 3] {}
    }

    test {A hash tombstone isn't set if an unknown primary is XDELed} {
        r DEL stash
        r XADD stash * id 1 temp 10
        r XHASH CREATE stash
        r XADD stash * id 2 temp 11
        set reply [r XHGET stash 1]
        assert_equal [llength $reply] 1
        assert_equal [lindex $reply 0 1] {}
        assert_equal [lindex $reply 0 3] {}
    }

    test {XHASH MAKE returns nil on keyspace miss} {
        r DEL stash
        set reply [r XHASH MAKE stash $]
        assert_equal $reply {}
    }

    test {XHASH MAKE on an never-used stream returns zero and an empty hash} {
        r DEL stash
        r XGROUP CREATE stash cg1 $ MKSTREAM
        set reply [r XHASH MAKE stash $]
        assert_equal $reply "0-0"
        set reply [r XHLEN stash]
        assert_equal $reply "0"
    }

    test {XHASH MAKE on an empty stream returns zero and an empty hash} {
        r DEL stash
        set id [r XADD stash * id 1 temp 10]
        r XDEL stash $id
        set reply [r XHASH MAKE stash $]
        assert_equal $reply "0-0"
        set reply [r XHLEN stash]
        assert_equal $reply "0"
    }

    test {XHASH MAKE on a simple stream} {
        r DEL stash
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 2 temp 20]
        set id3 [r XADD stash * id 1 temp 11]
        set id4 [r XADD stash * id 1 temp 12]
        set reply [r XHASH MAKE stash $]
        assert_equal $reply "0-0"
        set reply [r XHGETID stash 1 2]
        assert_equal [lindex $reply 0] $id4
        assert_equal [lindex $reply 1] $id2
        set reply [r XHLEN stash]
        assert_equal $reply "2"
    }

    test {XHASH MAKE on a stream with hash tombstone} {
        r DEL stash
        r XHASH CREATE stash MKSTREAM
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 2 temp 20]
        set id3 [r XADD stash * id 1 temp 11]
        set id4 [r XADD stash * id 1 temp 12]
        r XDEL stash $id4
        set reply [r XHASH MAKE stash $]
        assert_equal $reply "0-0"
        set reply [r XHGETID stash 1 2]
        assert_equal [lindex $reply 0] $id3
        assert_equal [lindex $reply 1] $id2
        set reply [r XHLEN stash]
        assert_equal $reply "2"
    }

    test {XHASH MAKE and MINID appear to work} {
        r DEL stash
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 2 temp 20]
        set id3 [r XADD stash * id 1 temp 11]
        set id4 [r XADD stash * id 1 temp 12]
        set reply [r XHASH MAKE stash $ MINID $id3]
        assert_equal $reply "0-0"
        set reply [r XHGETID stash 1 2]
        assert_equal [lindex $reply 0] $id4
        assert_equal [lindex $reply 1] {}
        set reply [r XHLEN stash]
        assert_equal $reply "1"
    }

    test {XHASH MAKE and a positive COUNT appear to work} {
        r DEL stash
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 2 temp 20]
        set id3 [r XADD stash * id 1 temp 11]
        set id4 [r XADD stash * id 1 temp 12]
        set reply [r XHASH MAKE stash $ COUNT 2]
        assert_equal $reply $id2
        set reply [r XHGETID stash 1 2]
        assert_equal [lindex $reply 0] $id4
        assert_equal [lindex $reply 1] {}
        set reply [r XHLEN stash]
        assert_equal $reply "1"
    }

    test {XHASH MAKE and a negative COUNT appear to work} {
        r DEL stash
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 2 temp 20]
        set id3 [r XADD stash * id 1 temp 11]
        set id4 [r XADD stash * id 1 temp 12]
        set reply [r XHASH MAKE stash $ COUNT -1]
        assert_equal $reply "0-0"
        set reply [r XHGETID stash 1 2]
        assert_equal [lindex $reply 0] $id4
        assert_equal [lindex $reply 1] $id2
        set reply [r XHLEN stash]
        assert_equal $reply "2"
   }

    test {XHASH MAKE ... COUNT 0 does nothing} {
        r DEL stash
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 2 temp 20]
        set id3 [r XADD stash * id 1 temp 11]
        set id4 [r XADD stash * id 1 temp 12]
        set reply [r XHASH MAKE stash $ COUNT 0]
        assert_equal $reply $id4
        set reply [r XHGETID stash 1 2]
        assert_equal [lindex $reply 0] {}
        assert_equal [lindex $reply 1] {}
        set reply [r XHLEN stash]
        assert_equal $reply "0"

        set reply [r XHASH MAKE stash $id2 COUNT 0]
        assert_equal $reply $id2
        set reply [r XHGETID stash 1 2]
        assert_equal [lindex $reply 0] {}
        assert_equal [lindex $reply 1] {}
        set reply [r XHLEN stash]
        assert_equal $reply "0"
   }

   test {XHASH VACUUM appears to work correcly} {
        r DEL stash
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 2 temp 20]
        set id3 [r XADD stash * id 1 temp 11]
        set id4 [r XADD stash * id 1 temp 12]
        r XHASH MAKE stash $
        set reply [r XHASH VACUUM stash]
        assert_equal $reply "0"

        r XDEL stash $id4
        set reply [r XHASH VACUUM stash]
        assert_equal $reply "1"
        set reply [r XHLEN stash]
        assert_equal $reply "1"
   }

   test {XHASH HELP should not have unexpected options} {
        catch {r XHASH HELP xxx} err
        assert_match "*ERR*" $err
   }

   test {XINFO STREAM/HASH return nil when there's no stream hash} {
        r DEL stash
        r XADD stash * id 1 temp 10
        set reply [r XINFO STREAM stash]
        assert_equal [lindex $reply 13] {}

        set reply [r XINFO HASH stash]
        assert_equal [lindex $reply 1] {}
        assert_equal [lindex $reply 3] {}
    }

   test {XINFO STREAM/HASH appear to work correctly} {
        r DEL stash
        r XADD stash * id 1 temp 10
        r XADD stash * id 2 temp 20
        r XADD stash * id 1 temp 11
        set id [r XADD stash * id 1 temp 12]
        r XHASH MAKE stash $
        set reply [r XINFO STREAM stash]
        assert_equal [lindex $reply 13] 2
        set reply [r XINFO HASH stash]
        assert_equal [lindex $reply 1] 2
        assert_equal [lindex $reply 3] 0

        r XDEL stash $id
        set reply [r XINFO STREAM stash]
        assert_equal [lindex $reply 13] 2
        set reply [r XINFO HASH stash]
        assert_equal [lindex $reply 1] 2
        assert_equal [lindex $reply 3] 1
    }

    test {XHCOMAPCT returns nil and doesn't create dst if source stream doesn't exist} {
        r DEL stash
        r DEL dst
        set reply [r XHCOMPACT dst stash]
        assert_equal $reply {}
        set reply [r EXISTS dst]
        assert_equal $reply "0"
    }

    test {XHCOMAPCT returns nil and doesn't create dst if source stream doesn't have a hash} {
        r DEL stash
        r DEL dst
        r XADD stash * id 1 temp 10
        set reply [r XHCOMPACT dst stash]
        assert_equal $reply {}
        set reply [r EXISTS dst]
        assert_equal $reply "0"
    }

    test {XHCOMAPCT appears to work on a simple stream} {
        r DEL stash
        r DEL dst
        r XHASH CREATE stash MKSTREAM
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 2 temp 20]
        set id3 [r XADD stash * id 1 temp 11]
        set id4 [r XADD stash * id 1 temp 12]
        set reply [r XHCOMPACT dst stash]
        assert_equal [llength $reply] 6
        assert_equal [lindex $reply 0] 4 ;# source stream length
        assert_equal [lindex $reply 1] 2 ;# source stream hash length
        assert_equal [lindex $reply 2] 0 ;# source stream number of hash tombs
        assert_equal [lindex $reply 3] 2 ;# destination stream length
        assert_equal [lindex $reply 4] 2 ;# destination stream hash length
        assert_equal [lindex $reply 5] 0 ;# destination stream number of hash tombs
        set reply [r XHLEN dst]
        assert_equal $reply 2
        set reply [r XHGET dst 1 2]
        assert_equal [lindex $reply 0 1] $id4
        assert_equal [lindex $reply 1 1] $id2
    }

    test {XHCOMAPCT appears to work on a riddled stream} {
        r DEL stash
        r DEL dst
        r XHASH CREATE stash MKSTREAM
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 2 temp 20]
        set id3 [r XADD stash * id 1 temp 11]
        set id4 [r XADD stash * id 1 temp 12]
        r XDEL stash $id4
        set reply [r XHCOMPACT dst stash]
        assert_equal [llength $reply] 6
        assert_equal [lindex $reply 0] 3 ;# source stream length
        assert_equal [lindex $reply 1] 2 ;# source stream hash length
        assert_equal [lindex $reply 2] 1 ;# source stream number of hash tombs
        assert_equal [lindex $reply 3] 1 ;# destination stream length
        assert_equal [lindex $reply 4] 1 ;# destination stream hash length
        assert_equal [lindex $reply 5] 0 ;# destination stream number of hash tombs
        set reply [r XHLEN dst]
        assert_equal $reply 1
        set reply [r XHGET dst 1 2]
        assert_equal [lindex $reply 0 1] {}
        assert_equal [lindex $reply 1 1] $id2

        r XDEL stash $id2
        set reply [r XHCOMPACT dst stash]
        assert_equal [llength $reply] 6
        assert_equal [lindex $reply 0] 2 ;# source stream length
        assert_equal [lindex $reply 1] 2 ;# source stream hash length
        assert_equal [lindex $reply 2] 2 ;# source stream number of hash tombs
        assert_equal [lindex $reply 3] 0 ;# destination stream length
        assert_equal [lindex $reply 4] 0 ;# destination stream hash length
        assert_equal [lindex $reply 5] 0 ;# destination stream number of hash tombs
        set reply [r XHLEN dst]
        assert_equal $reply 0
        set reply [r XHGET dst 1 2]
        assert_equal [lindex $reply 0 1] {}
        assert_equal [lindex $reply 1 1] {}
    }

    test {XHCOMAPCT WITHNULLs appears to work} {
        r DEL stash
        r DEL dst
        r XHASH CREATE stash MKSTREAM
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 2 temp 20]
        set id3 [r XADD stash * id 1 temp 11]
        set id4 [r XADD stash * id 1 temp 12]
        r XDEL stash $id4
        set reply [r XHCOMPACT dst stash WITHNULLS]
        assert_equal [llength $reply] 6
        assert_equal [lindex $reply 0] 3 ;# source stream length
        assert_equal [lindex $reply 1] 2 ;# source stream hash length
        assert_equal [lindex $reply 2] 1 ;# source stream number of hash tombs
        assert_equal [lindex $reply 3] 1 ;# destination stream length
        assert_equal [lindex $reply 4] 2 ;# destination stream hash length
        assert_equal [lindex $reply 5] 1 ;# destination stream number of hash tombs
        set reply [r XHLEN dst]
        assert_equal $reply 2
        set reply [r XHGET dst 1 2]
        assert_equal [lindex $reply 0 1] "0-0"
        assert_equal [lindex $reply 1 1] $id2
    }

    test {XHCOMAPCT NOHASH appears to work} {
        r DEL stash
        r DEL dst
        r XHASH CREATE stash MKSTREAM
        r XADD stash * id 1 temp 10
        r XADD stash * id 2 temp 20
        r XADD stash * id 1 temp 11
        r XADD stash * id 1 temp 12
        set reply [r XHCOMPACT dst stash NOHASH]
        assert_equal [llength $reply] 6
        assert_equal [lindex $reply 0] 4 ;# source stream length
        assert_equal [lindex $reply 1] 2 ;# source stream hash length
        assert_equal [lindex $reply 2] 0 ;# source stream number of hash tombs
        assert_equal [lindex $reply 3] 2 ;# destination stream length
        assert_equal [lindex $reply 4] {} ;# destination stream hash length
        assert_equal [lindex $reply 5] {} ;# destination stream number of hash tombs
        set reply [r XHLEN dst]
        assert_equal $reply {}
    }

    test {XHCOMAPCT appears to work when src and dst are the same} {
        r DEL stash
        r XHASH CREATE stash MKSTREAM
        r XADD stash * id 1 temp 10
        r XADD stash * id 2 temp 20
        r XADD stash * id 1 temp 11
        r XADD stash * id 1 temp 12
        set reply [r XHCOMPACT stash stash]
        assert_equal [llength $reply] 6
        assert_equal [lindex $reply 0] 4 ;# source stream length
        assert_equal [lindex $reply 1] 2 ;# source stream hash length
        assert_equal [lindex $reply 2] 0 ;# source stream number of hash tombs
        assert_equal [lindex $reply 3] 2 ;# destination stream length
        assert_equal [lindex $reply 4] 2 ;# destination stream hash length
        assert_equal [lindex $reply 5] 0 ;# destination stream number of hash tombs
        set reply [r XHLEN dst]
        assert_equal $reply {}
    }

    test {XHCOMPACT errors if NOHASH and WITHNULLS are used together} {
        r DEL stash
        r DEL dst
        r XHASH CREATE stash MKSTREAM
        catch {r XHCOMPACT dst stash NOHASH WITHNULLS} err
        set _ $err
    } {*ERR*together*}

    test {XHSCAN returns an empty scan for a keyspace miss} {
        r DEL stash
        set reply [r XHSCAN stash 0]
        assert_equal [lindex $reply 0] "0"
        assert_equal [lindex $reply 1] {}
    }

    test {XHSCAN returns an empty scan for a stream without a hash} {
        r DEL stash
        r XADD stash * id 1 temp 10
        set reply [r XHSCAN stash 0]
        assert_equal [lindex $reply 0] "0"
        assert_equal [lindex $reply 1] {}
    }

    test {XHSCAN appears to scan correctly} {
        r DEL stash
        r DEL dst
        r XHASH CREATE stash MKSTREAM
        set id1 [r XADD stash * id 1 temp 10]
        set id2 [r XADD stash * id 2 temp 20]
        set id3 [r XADD stash * id 1 temp 11]
        set id4 [r XADD stash * id 1 temp 12]
        set reply [r XHSCAN stash 0]
        assert_equal [lindex $reply 0] 0
        assert_equal [llength [lindex $reply 1]] 4
    }
}