# Check cluster info stats

source "../tests/includes/init-tests.tcl"

test "Create a primary with a replica" {
    create_cluster 2 0
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

set primary1 [Rn 0]
set primary2 [Rn 1]

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
    # we know that the primary 2 will have a MOVED reply
    catch {$primary1 set key{0x0000} b} replyP1
    catch {$primary2 set key{0x0000} b} replyP2
    assert_match {OK} $replyP1
    assert_match {} [errorstat $primary1 MOVED]
    assert_match {*count=1*} [errorstat $primary2 MOVED]
    assert_match {*calls=0,*,rejected_calls=1,failed_calls=0} [cmdstat $primary2 set]
}
