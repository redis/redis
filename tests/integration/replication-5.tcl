start_server {tags {"repl"}} {
    start_server {} {
        test {slave-sync-speed-bps should be honored} {
            # ~ 550Kb
            createComplexDataset r 10000
            after 500

            # we sync at 500 Kb/s -> this should finish after 1.1s
            r -1 config set slave-sync-speed-bps 4000000
            r -1 slaveof [srv 0 host] [srv 0 port]

            after 500
            # slave should not be ready in 0.5s
            assert {[s -1 master_link_status] eq {down}}

            wait_for_condition 20 100 {
                [s -1 master_link_status] eq {up}
            } else {
                fail "Slave didn't sync in time"
            }
        }
    }
}
