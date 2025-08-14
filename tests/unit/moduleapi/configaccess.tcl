set testmodule [file normalize tests/modules/configaccess.so]
set othermodule [file normalize tests/modules/moduleconfigs.so]

start_server {tags {"modules"}} {
    r module load $testmodule
    r module loadex $othermodule CONFIG moduleconfigs.mutable_bool yes

    test {Test module config get with standard Redis configs} {
        # Test getting standard Redis configs of different types
        set maxmemory [r config get maxmemory]
        assert_equal [lindex $maxmemory 1] [r configaccess.getnumeric maxmemory]

        set port [r config get port]
        assert_equal [lindex $port 1] [r configaccess.getnumeric port]

        set appendonly [r config get appendonly]
        assert_equal [string is true [lindex $appendonly 1]] [r configaccess.getbool appendonly]

        # Test string config
        set logfile [r config get logfile]
        assert_equal [lindex $logfile 1] [r configaccess.get logfile]

        # Test SDS config
        set requirepass [r config get requirepass]
        assert_equal [lindex $requirepass 1] [r configaccess.get requirepass]

        # Test special config
        set oom_score_adj_values [r config get oom-score-adj-values]
        assert_equal [lindex $oom_score_adj_values 1] [r configaccess.get oom-score-adj-values]

        set maxmemory_policy_name [r configaccess.getenum maxmemory-policy]
        assert_equal [lindex [r config get maxmemory-policy] 1] $maxmemory_policy_name

        # Test percent config
        r config set maxmemory 100000
        r configaccess.setnumeric maxmemory-clients -50
        assert_equal [lindex [r config get maxmemory-clients] 1] 50%

        # Test multi-argument enum config
        r config set moduleconfigs.flags "one two four"
        assert_equal "five two" [r configaccess.getenum moduleconfigs.flags]

        # Test getting multi-argument enum config via generic get
        r config set moduleconfigs.flags "two four"
        assert_equal "two four" [r configaccess.get moduleconfigs.flags]
    }

    test {Test module config get with non-existent configs} {
        # Test getting non-existent configs
        catch {r configaccess.getnumeric nonexistent_config} err
        assert_match "ERR*" $err

        catch {r configaccess.getbool nonexistent_config} err
        assert_match "ERR*" $err

        catch {r configaccess.get nonexistent_config} err
        assert_match "ERR*" $err

        catch {r configaccess.getenum nonexistent_config} err
        assert_match "ERR*" $err
    }

    test {Test module config set with standard Redis configs} {
        # Test setting numeric config
        set old_maxmemory_samples [r config get maxmemory-samples]
        r configaccess.setnumeric maxmemory-samples 10
        assert_equal "maxmemory-samples 10" [r config get maxmemory-samples]
        r config set maxmemory-samples [lindex $old_maxmemory_samples 1]

        # Test setting bool config
        set old_protected_mode [r config get protected-mode]
        r configaccess.setbool protected-mode no
        assert_equal "protected-mode no" [r config get protected-mode]
        r config set protected-mode [lindex $old_protected_mode 1]

        # Test setting string config
        set old_masteruser [r config get masteruser]
        r configaccess.set masteruser "__newmasteruser__"
        assert_equal "__newmasteruser__" [lindex [r config get masteruser] 1]
        r config set masteruser [lindex $old_masteruser 1]

        # Test setting enum config
        set old_loglevel [r config get loglevel]
        r config set loglevel "notice" ; # Set to some value we are sure is different than the one tested
        r configaccess.setenum loglevel warning
        assert_equal "loglevel warning" [r config get loglevel]
        r config set loglevel [lindex $old_loglevel 1]

        # Test setting multi-argument enum config
        r config set moduleconfigs.flags "one two four"
        assert_equal "moduleconfigs.flags {five two}" [r config get moduleconfigs.flags]
        r configaccess.setenum moduleconfigs.flags "two four"
        assert_equal "moduleconfigs.flags {two four}" [r config get moduleconfigs.flags]

        # Test setting multi-argument enum config via generic set
        r config set moduleconfigs.flags "one two four"
        assert_equal "moduleconfigs.flags {five two}" [r config get moduleconfigs.flags]
        r configaccess.set moduleconfigs.flags "two four"
        assert_equal "moduleconfigs.flags {two four}" [r config get moduleconfigs.flags]
    }

    test {Test module config set with module configs} {
        # Test setting module bool config
        assert_equal "OK" [r configaccess.setbool configaccess.bool no]
        assert_equal "configaccess.bool no" [r config get configaccess.bool]

        # Test setting module bool config from another module
        assert_equal "OK" [r configaccess.setbool moduleconfigs.mutable_bool no]
        assert_equal "moduleconfigs.mutable_bool no" [r config get moduleconfigs.mutable_bool]

        # Test setting module numeric config
        assert_equal "OK" [r configaccess.setnumeric moduleconfigs.numeric 100]
        assert_equal "moduleconfigs.numeric 100" [r config get moduleconfigs.numeric]

        # Test setting module enum config
        assert_equal "OK" [r configaccess.setenum moduleconfigs.enum "five"]
        assert_equal "moduleconfigs.enum five" [r config get moduleconfigs.enum]
    }

    test {Test module config set with error cases} {
        # Test setting a non-existent config
        catch {r configaccess.setbool nonexistent_config yes} err
        assert_match "*ERR*" $err

        # Test setting a read-only config
        catch {r configaccess.setbool moduleconfigs.immutable_bool yes} err
        assert_match "*ERR*" $err

        # Test setting an enum config with invalid value
        catch {r configaccess.setenumname moduleconfigs.enum "invalid_value"} err
        assert_match "*ERR*" $err

        # Test setting a numeric config with out-of-range value
        catch {r configaccess.setnumeric moduleconfigs.numeric 5000} err
        assert_match "*ERR*" $err
    }

    test {Test module get all configs} {
        # Get all configs using the module command
        set all_configs [r configaccess.getconfigs]

        # Verify the number of configs matches the number of configs returned
        # by Redis's native CONFIG GET command.
        set all_configs_std_pairs [llength [r config get *]]

        # When comparing with the standard CONFIG GET command, we need to divide
        # by 2 because the standard command returns a flattened array of
        # key-value pairs whereas our testing command returns an array of pairs.
        assert_equal [llength $all_configs] [expr $all_configs_std_pairs / 2]

        # Verify all the configs are present in both replies.
        foreach config_pair $all_configs {
            assert_equal 2 [llength $config_pair]
            set config_name [lindex $config_pair 0]
            set config_value [lindex $config_pair 1]

            # Verify that we can get this config using standard config get
            set redis_config [r config get $config_name]
            assert {[llength $redis_config] != 0}

            assert_equal $config_value [lindex $redis_config 1]
        }

        # Test that module configs are also included
        set found_module_config 0
        foreach config_pair $all_configs {
            set config_name [lindex $config_pair 0]
            if {$config_name eq "configaccess.bool"} {
                set found_module_config 1
                break
            }
        }

        assert {$found_module_config == 1}

        # Test pattern matching
        set moduleconfigs_count [r configaccess.getconfigs "moduleconfigs.*"]
        assert_equal 7 [llength $moduleconfigs_count]

        set memoryconfigs_count [r configaccess.getconfigs "*memory"]
        assert_equal 3 [llength $memoryconfigs_count]
    }

    test {Test module config type detection} {
        # Test getting config types for different config types
        assert_equal "bool" [r configaccess.getconfigtype appendonly]
        assert_equal "numeric" [r configaccess.getconfigtype port]
        assert_equal "string" [r configaccess.getconfigtype logfile]
        assert_equal "enum" [r configaccess.getconfigtype maxmemory-policy]

        # Test with module config
        assert_equal "bool" [r configaccess.getconfigtype configaccess.bool]

        # Test with non-existent config
        catch {r configaccess.getconfigtype nonexistent_config} err
        assert_match "ERR Config does not exist" $err
    }

    test {Test config rollback on apply} {
        set og_port [lindex [r config get port] 1]

        set used_port [find_available_port $::baseport $::portcount]

        # Run a dummy server on used_port so we know we can't configure redis to 
        # use it. It's ok for this to fail because that means used_port is invalid 
        # anyway
        catch {set sockfd [socket -server dummy_accept -myaddr 127.0.0.1 $used_port]} e
        if {$::verbose} { puts "dummy_accept: $e" }

        # Try to listen on the used port, pass some more configs to make sure the
        # returned failure message is for the first bad config and everything is rolled back.
        assert_error "ERR Failed to set numeric config port: Unable to listen on this port*" {
            eval "r configaccess.setnumeric port $used_port"
        }

        assert_equal [lindex [r config get port] 1] $og_port
        close $sockfd
    }
}
