# Check cluster info stats

source "../tests/includes/init-tests.tcl"

test "Create a primary with a replica" {
    create_cluster 2 2
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

set primary1 [Rn 0]
set primary2 [Rn 1]
set replica1 [Rn 2]
set replica2 [Rn 3]

proc cmdstat {instace cmd} {
    return [cmdrstat $cmd $instace]
}

proc errorstat {instace cmd} {
    return [errorrstat $cmd $instace]
}

test "errorstats: rejected call due to MOVED Redirection" {
    $primary1 config resetstat
    $primary2 config resetstat
    assert_match {} [errorstat $primary1 MOVED]
    assert_match {} [errorstat $primary2 MOVED]
    # we know that one of the commands will have a MOVED reply
    catch {$primary1 set a b} replyP1
    catch {$primary2 set a b} replyP2
    if { [string range $replyP1 0 4] eq {MOVED} } {
        assert_match {*count=1*} [errorstat $primary1 MOVED]
        assert_match {*calls=0,*,rejected_calls=1,failed_calls=0} [cmdstat $primary1 set]
        assert_match {OK} $replyP2
        assert_match {} [errorstat $primary2 MOVED]
    }
    if { [string range $replyP2 0 4] eq {MOVED} } {
        assert_match {*count=1*} [errorstat $primary2 MOVED]
        assert_match {*calls=0,*,rejected_calls=1,failed_calls=0} [cmdstat $primary2 set]
        assert_match {OK} $replyP1
        assert_match {} [errorstat $primary1 MOVED]
    }
}
