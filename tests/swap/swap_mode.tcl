test "start swap disk mode + aof" {
    set server_path [tmpdir "server.swap_mode-test"]
    set stdout [format "%s/%s" $server_path "stdout"]
    set stderr [format "%s/%s" $server_path "stderr"]
    set srv [exec src/redis-server --swap-mode disk --appendonly yes >> $stdout 2>> $stderr &]
    wait_for_condition 100 20 {
        [regexp -- "FATAL CONFIG FILE ERROR" [exec cat $stderr]] == 1 &&
        [regexp -- ">>> 'appendonly \"yes\"'" [exec cat $stderr]] == 1
    } else {
        fail "swap disk + aof start success"
        kill_server $srv
    }
}

test "start swap disk aof + mode" {
    set server_path [tmpdir "server.swap_mode-test-2"]
    set stdout [format "%s/%s" $server_path "stdout"]
    set stderr [format "%s/%s" $server_path "stderr"]
    set srv [exec src/redis-server --appendonly yes --swap-mode disk  >> $stdout 2>> $stderr &]
    wait_for_condition 100 20 {
        [regexp -- "FATAL CONFIG FILE ERROR" [exec cat $stderr]] == 1 &&
        [regexp -- ">>> 'swap-mode \"disk\"'" [exec cat $stderr]] == 1
    } else {
        fail "aof + swap disk  start success"
        kill_server $srv
    }
}

# # swap-mode can't change
# test "runing server when open aof ,change swap_mode to disk fail" {
#     start_server {overrides {appendonly {yes}}} {
#         catch {r config set swap-mode disk} error 
#         assert_equal [string match {*ERR Invalid argument 'disk' for CONFIG SET 'swap-mode'*} $error] 1
#     }
# }

test "runing server when swap_mode == disk ,open aof fail" {
    start_server {overrides {appendonly {no} swap-mode {disk} }} {
        catch {r config set appendonly yes} error 
        assert_equal [string match {*ERR Invalid argument 'yes' for CONFIG SET 'appendonly'*} $error] 1
        assert_equal [r dbsize] 0
    }
}


test "runing server swap-mode disk=>memory fail" {
    start_server {overrides {swap-mode {memory} }} {
        catch {r config set swap-mode disk} error 
        assert_match "ERR Unsupported CONFIG parameter: swap-mode" $error 
        assert_equal [lindex [r config get swap-mode] 1] {memory}
    }
}

test "runing server swap-mode memory=>disk fail" {
    start_server {overrides {swap-mode {disk} }} {
        catch {r config set swap-mode memory} error 
        assert_match "ERR Unsupported CONFIG parameter: swap-mode" $error 
        assert_equal [lindex [r config get swap-mode] 1] {disk}
    }
} 
