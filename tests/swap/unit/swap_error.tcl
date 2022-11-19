start_server {tags {"swap error"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set slave [srv 0 client]

        $master config set swap-debug-evict-keys 0
        $slave config set swap-debug-evict-keys 0
        $slave replicaof $master_host $master_port
        wait_for_sync $slave

        test {swap error if rio failed} {
            $master set key value

            $master swap rio-error 1
            $master evict key
            after 100
            assert ![object_is_cold $master key]
            assert {[get_info $master swaps swap_error] eq 1}

            $slave swap rio-error 1
            $slave evict key
            after 100
            assert ![object_is_cold $slave key]
            assert {[get_info $slave swaps swap_error] eq 1}

            $master evict key
            wait_key_cold $master key
            assert [object_is_cold $master key]
            assert {[get_info $master swaps swap_error] eq 1}

            $slave evict key
            wait_key_cold $slave key
            assert [object_is_cold $slave key]
            assert {[get_info $slave swaps swap_error] eq 1}

            $master swap rio-error 1
            catch {$master get key} {e}
            assert_match {*Swap failed*} $e
            assert_equal [$master get key] value

            $slave swap rio-error 1
            catch {$slave get key} {e}
            assert_match {*Swap failed*} $e
            assert_equal [$slave get key] value

            $master swap rio-error 1
            catch {$master del key} {e}
            assert_match {*Swap failed*} $e
            after 100
            assert_equal [$master get key] value
            assert_equal [$slave get key] value

            $master del key
            after 100
            assert_equal [$master get key] {}
            assert_equal [$slave get key] {}
        }
    }
}
