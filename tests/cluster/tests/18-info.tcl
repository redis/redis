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
    # we know that one will have a MOVED reply and one will succeed
    catch {$primary1 set key b} replyP1
    catch {$primary2 set key b} replyP2
    # sort servers so we know which one failed
    if {$replyP1 eq {OK}} {
        assert_match {MOVED*} $replyP2
        set pok $primary1
        set perr $primary2
    } else {
        assert_match {MOVED*} $replyP1
        set pok $primary2
        set perr $primary1
    }
    assert_match {} [errorstat $pok MOVED]
    assert_match {*count=1*} [errorstat $perr MOVED]
    assert_match {*calls=0,*,rejected_calls=1,failed_calls=0} [cmdstat $perr set]
}
