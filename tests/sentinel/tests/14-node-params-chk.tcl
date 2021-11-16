# Check the basic monitoring and failover capabilities.
source "../tests/includes/init-tests.tcl"


test "check sentinel master params" {
    #test without any parameter
    set info [S 0 SENTINEL master mymaster]
    assert { [string match "*link-refcount*" $info] }
    assert { [string match "*info-refresh*" $info] }
    assert { [string match "*quorum*" $info] }
    assert { ![string match "*master-link-status*" $info] }
    assert { ![string match "*last-hello-message*" $info] }

    set param1 "flags"
    set param2 "role-reported"
    set param3 "num-slaves"

    #test with one parameter
    set info [S 0 SENTINEL master mymaster $param1]
    assert { [string match "*flags*" $info] }
    assert { ![string match "*info-refresh*" $info] }
    assert { ![string match "*quorum*" $info] }

    #test with multiple parameters
    set info [S 0 SENTINEL master mymaster $param1 $param2 $param3]
    assert { [string match "*flags*" $info] }
    assert { [string match "*role-reported*" $info] }
    assert { [string match "*num-slaves*" $info] }
    assert { ![string match "*info-refresh*" $info] }
    assert { ![string match "*quorum*" $info] }
}

test "check sentinel replicas params" {
    set param1 "config-epoch"
    set param2 "master-link-status"
    set param3 "master-port"

    set info [S 0 SENTINEL replicas mymaster $param1 $param2 $param3]
    assert { ![string match "*config-epoch*" $info] }
    assert { [string match "*master-link-status*" $info] }
    assert { [string match "*master-port*" $info] }
}

test "check sentinel sentinels params" {
    set param1 "info-refresh"
    set param2 "last-hello-message"
    set param3 "master-port"

    set info [S 0 SENTINEL sentinels mymaster $param1 $param2 $param3]
    assert { ![string match "*info-refresh*" $info] }
    assert { [string match "*last-hello-message*" $info] }
    assert { ![string match "*master-port*" $info] }
}
