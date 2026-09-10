# Verify "sentinel state-config-file": Sentinel persists its runtime state into
# a separate file and never rewrites the (administrator managed) main config
# file. See https://github.com/redis/redis/issues/5226.
#
# This test spawns its own dedicated Sentinel process (independent from the
# shared test fleet) so it can fully control both config files.

set ::sentinel_bin "../../../src/redis-sentinel"

proc scf_read_file {path} {
    if {![file exists $path]} { return "" }
    set fd [open $path r]
    set data [read $fd]
    close $fd
    return $data
}

# Start a dedicated Sentinel from a config template containing a "%PORT%"
# placeholder. find_available_port() may hand back a port that a fleet instance
# already bound (the probe socket can bind over it while a real bind fails), so
# retry with a fresh port until our own process is actually up and listening.
# Returns [list pid port].
proc scf_launch {conf_path conf_template base_dir} {
    for {set attempt 0} {$attempt < 50} {incr attempt} {
        set port [find_available_port $::sentinel_base_port $::redis_port_count]
        set fd [open $conf_path w]
        puts $fd [string map [list %PORT% $port] $conf_template]
        close $fd
        set pid [exec $::sentinel_bin $conf_path 2> [file join $base_dir stderr.txt] &]
        for {set t 0} {$t < 30} {incr t} {
            if {[catch {exec kill -0 $pid}]} break ;# process died (e.g. bind failed)
            # Confirm the server answering on this port is *our* process and not
            # a fleet instance that already owns the port (in which case our
            # process is about to exit with a bind error).
            if {![catch {set l [redis 127.0.0.1 $port 0 0]}]} {
                set gotpid -1
                catch {regexp {process_id:(\d+)} [$l info server] -> gotpid}
                catch {$l close}
                if {$gotpid == $pid} { return [list $pid $port] }
            }
            after 100
        }
        catch {exec kill -9 $pid}
    }
    fail "could not start state-config-file sentinel"
    return {}
}

if {$::tls} {
    puts "Skipping state-config-file test under TLS"
} else {
    set base_dir [file normalize "state-config-file-test"]
    catch {exec rm -rf $base_dir}
    file mkdir $base_dir

    set main_conf [file join $base_dir "sentinel.conf"]
    set state_conf [file join $base_dir "sentinel-state.conf"]

    set conf_template [subst -nocommands {port %PORT%
dir $base_dir
logfile log.txt
enable-protected-configs yes
enable-debug-command yes
sentinel state-config-file $state_conf
sentinel monitor mymaster 127.0.0.1 12345 2
sentinel down-after-milliseconds mymaster 20000}]

    lassign [scf_launch $main_conf $conf_template $base_dir] pid port
    set main_conf_orig [scf_read_file $main_conf]

    # Wrap the lifecycle in a catch so that the cleanup below always runs and
    # kills the spawned process, even if a non-assertion error occurs (assertion
    # failures inside "test" are already handled by the test framework).
    catch {
        set link [redis 127.0.0.1 $port 0 0]
        $link reconnect 1

        test "state-config-file: state is written to the separate file, not the main config" {
            # On startup Sentinel picks a myid and flushes state to disk.
            wait_for_condition 50 100 {
                [string match "*sentinel myid*" [scf_read_file $state_conf]]
            } else {
                fail "Sentinel did not create the state config file"
            }
            set state [scf_read_file $state_conf]
            assert_match "*sentinel myid*" $state
            assert_match "*sentinel monitor mymaster*" $state
            assert_match "*sentinel current-epoch*" $state

            # The main config file must be left untouched: no runtime state added.
            assert_equal $main_conf_orig [scf_read_file $main_conf]
        }

        set saved_myid [$link sentinel myid]

        test "state-config-file: SENTINEL SET is persisted to the state file only" {
            $link sentinel set mymaster down-after-milliseconds 12345
            wait_for_condition 50 100 {
                [string match "*down-after-milliseconds mymaster 12345*" [scf_read_file $state_conf]]
            } else {
                fail "SENTINEL SET was not persisted to the state config file"
            }
            # Main config file is still untouched.
            assert_equal $main_conf_orig [scf_read_file $main_conf]
        }

        test "state-config-file: runtime state is restored after restart" {
            catch {$link close}
            exec kill $pid
            wait_for_condition 50 100 {
                [catch {exec kill -0 $pid}] == 1
            } else {
                fail "Sentinel process did not terminate"
            }

            # Restart with the same config (and port); the port is free again.
            set pid [exec $::sentinel_bin $main_conf 2> [file join $base_dir stderr.txt] &]
            set link ""
            wait_for_condition 50 100 {
                ![catch {set link [redis 127.0.0.1 $port 0 0]; $link ping}]
            } else {
                fail "Sentinel did not restart"
            }
            $link reconnect 1

            # The Sentinel id is stable across restarts (loaded from the state file).
            assert_equal $saved_myid [$link sentinel myid]

            # The SENTINEL SET override survived the restart.
            set master [$link sentinel master mymaster]
            assert_equal 12345 [dict get $master down-after-milliseconds]
        }
    }

    catch {$link close}
    catch {exec kill $pid}
    catch {exec rm -rf $base_dir}

    # Migration case: the same runtime directives are present in both the main
    # config file (e.g. a pre-split combined config) and the state file. Sentinel
    # must boot without failing on duplicates, with the state file taking
    # precedence.
    set base_dir [file normalize "state-config-file-dup-test"]
    catch {exec rm -rf $base_dir}
    file mkdir $base_dir
    set main_conf [file join $base_dir "sentinel.conf"]
    set state_conf [file join $base_dir "sentinel-state.conf"]

    # Pre-existing state file overlapping the main config (different current
    # address and higher epoch). Written before the sentinel starts.
    set fd [open $state_conf w]
    puts $fd "sentinel myid 0123456789abcdef0123456789abcdef01234567"
    puts $fd "sentinel monitor mymaster 127.0.0.1 12399 2"
    puts $fd "sentinel known-replica mymaster 127.0.0.1 12346"
    puts $fd "sentinel config-epoch mymaster 7"
    puts $fd "sentinel current-epoch 7"
    close $fd

    set conf_template [subst -nocommands {port %PORT%
dir $base_dir
logfile log.txt
sentinel state-config-file $state_conf
sentinel monitor mymaster 127.0.0.1 12345 2
sentinel known-replica mymaster 127.0.0.1 12346
sentinel config-epoch mymaster 3}]

    lassign [scf_launch $main_conf $conf_template $base_dir] pid port

    catch {
        test "state-config-file: duplicate directives across files are tolerated" {
            set link [redis 127.0.0.1 $port 0 0]
            $link reconnect 1

            # myid comes from the state file.
            assert_equal "0123456789abcdef0123456789abcdef01234567" [$link sentinel myid]

            set master [$link sentinel master mymaster]
            # Address and epoch reflect the state file (loaded last / wins).
            assert_equal 12399 [dict get $master port]
            assert_equal 7 [dict get $master config-epoch]
            # The single replica is known exactly once.
            assert_equal 1 [dict get $master num-slaves]
            catch {$link close}
        }
    }

    catch {exec kill $pid}
    catch {exec rm -rf $base_dir}

    # A state file pointing at the main config file is a misconfiguration and
    # Sentinel must refuse to start. The guard runs before the port is bound, so
    # a port collision cannot mask it.
    test "state-config-file: refuses to point at the main config file" {
        set d [file normalize "state-config-file-same-test"]
        catch {exec rm -rf $d}
        file mkdir $d
        set conf [file join $d "sentinel.conf"]
        set p [find_available_port $::sentinel_base_port $::redis_port_count]
        set fd [open $conf w]
        puts $fd "port $p"
        puts $fd "dir $d"
        puts $fd "logfile log.txt"
        puts $fd "sentinel state-config-file $conf"
        puts $fd "sentinel monitor mymaster 127.0.0.1 12345 2"
        close $fd

        set spid [exec $::sentinel_bin $conf 2> [file join $d stderr.txt] &]
        wait_for_condition 50 100 {
            [catch {exec kill -0 $spid}] == 1
        } else {
            fail "Sentinel did not refuse to start with state-config-file == main config"
        }
        assert_match "*must differ from the main config file*" \
            "[scf_read_file [file join $d log.txt]][scf_read_file [file join $d stderr.txt]]"
        catch {exec kill $spid}
        catch {exec rm -rf $d}
    }
}
