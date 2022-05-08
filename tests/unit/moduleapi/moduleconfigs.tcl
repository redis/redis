set testmodule [file normalize tests/modules/moduleconfigs.so]
set testmoduletwo [file normalize tests/modules/moduleconfigstwo.so]

start_server {tags {"modules"}} {
    r module load $testmodule
    test {Config get commands work} {
        # Make sure config get module config works
        assert_equal [lindex [lindex [r module list] 0] 1] moduleconfigs
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool yes"
        assert_equal [r config get moduleconfigs.immutable_bool] "moduleconfigs.immutable_bool no"
        assert_equal [r config get moduleconfigs.memory_numeric] "moduleconfigs.memory_numeric 1024"
        assert_equal [r config get moduleconfigs.string] "moduleconfigs.string {secret password}"
        assert_equal [r config get moduleconfigs.enum] "moduleconfigs.enum one"
        assert_equal [r config get moduleconfigs.flags] "moduleconfigs.flags {one two}"
        assert_equal [r config get moduleconfigs.numeric] "moduleconfigs.numeric -1"
    }

    test {Config set commands work} {
        # Make sure that config sets work during runtime
        r config set moduleconfigs.mutable_bool no
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool no"
        r config set moduleconfigs.memory_numeric 1mb
        assert_equal [r config get moduleconfigs.memory_numeric] "moduleconfigs.memory_numeric 1048576"
        r config set moduleconfigs.string wafflewednesdays
        assert_equal [r config get moduleconfigs.string] "moduleconfigs.string wafflewednesdays"
        set not_embstr [string repeat A 50]
        r config set moduleconfigs.string $not_embstr
        assert_equal [r config get moduleconfigs.string] "moduleconfigs.string $not_embstr"
        r config set moduleconfigs.string \x73\x75\x70\x65\x72\x20\x00\x73\x65\x63\x72\x65\x74\x20\x70\x61\x73\x73\x77\x6f\x72\x64
        assert_equal [r config get moduleconfigs.string] "moduleconfigs.string {super \0secret password}"
        r config set moduleconfigs.enum two
        assert_equal [r config get moduleconfigs.enum] "moduleconfigs.enum two"
        r config set moduleconfigs.flags two
        assert_equal [r config get moduleconfigs.flags] "moduleconfigs.flags two"
        r config set moduleconfigs.numeric -2
        assert_equal [r config get moduleconfigs.numeric] "moduleconfigs.numeric -2"
    }

    test {Config set commands enum flags} {
        r config set moduleconfigs.flags "none"
        assert_equal [r config get moduleconfigs.flags] "moduleconfigs.flags none"

        r config set moduleconfigs.flags "two four"
        assert_equal [r config get moduleconfigs.flags] "moduleconfigs.flags {two four}"

        r config set moduleconfigs.flags "five"
        assert_equal [r config get moduleconfigs.flags] "moduleconfigs.flags five"

        r config set moduleconfigs.flags "one four"
        assert_equal [r config get moduleconfigs.flags] "moduleconfigs.flags five"

        r config set moduleconfigs.flags "one two four"
        assert_equal [r config get moduleconfigs.flags] "moduleconfigs.flags {five two}"
    }

    test {Immutable flag works properly and rejected strings dont leak} {
        # Configs flagged immutable should not allow sets
        catch {[r config set moduleconfigs.immutable_bool yes]} e
        assert_match {*can't set immutable config*} $e
        catch {[r config set moduleconfigs.string rejectisfreed]} e
        assert_match {*Cannot set string to 'rejectisfreed'*} $e
    }
    
    test {Numeric limits work properly} {
        # Configs over/under the limit shouldn't be allowed, and memory configs should only take memory values
        catch {[r config set moduleconfigs.memory_numeric 200gb]} e
        assert_match {*argument must be between*} $e
        catch {[r config set moduleconfigs.memory_numeric -5]} e
        assert_match {*argument must be a memory value*} $e
        catch {[r config set moduleconfigs.numeric -10]} e
        assert_match {*argument must be between*} $e
    }

    test {Enums only able to be set to passed in values} {
        # Module authors specify what values are valid for enums, check that only those values are ok on a set
        catch {[r config set moduleconfigs.enum asdf]} e
        assert_match {*must be one of the following*} $e
    }

    test {Unload removes module configs} {
        r module unload moduleconfigs
        assert_equal [r config get moduleconfigs.*] ""
        r module load $testmodule
        # these should have reverted back to their module specified values
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool yes"
        assert_equal [r config get moduleconfigs.immutable_bool] "moduleconfigs.immutable_bool no"
        assert_equal [r config get moduleconfigs.memory_numeric] "moduleconfigs.memory_numeric 1024"
        assert_equal [r config get moduleconfigs.string] "moduleconfigs.string {secret password}"
        assert_equal [r config get moduleconfigs.enum] "moduleconfigs.enum one"
        assert_equal [r config get moduleconfigs.flags] "moduleconfigs.flags {one two}"
        assert_equal [r config get moduleconfigs.numeric] "moduleconfigs.numeric -1"
        r module unload moduleconfigs
    }

    test {test loadex functionality} {
        r module loadex $testmodule CONFIG moduleconfigs.mutable_bool no CONFIG moduleconfigs.immutable_bool yes CONFIG moduleconfigs.memory_numeric 2mb CONFIG moduleconfigs.string tclortickle
        assert_equal [lindex [lindex [r module list] 0] 1] moduleconfigs
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool no"
        assert_equal [r config get moduleconfigs.immutable_bool] "moduleconfigs.immutable_bool yes"
        assert_equal [r config get moduleconfigs.memory_numeric] "moduleconfigs.memory_numeric 2097152"
        assert_equal [r config get moduleconfigs.string] "moduleconfigs.string tclortickle"
        # Configs that were not changed should still be their module specified value
        assert_equal [r config get moduleconfigs.enum] "moduleconfigs.enum one"
        assert_equal [r config get moduleconfigs.flags] "moduleconfigs.flags {one two}"
        assert_equal [r config get moduleconfigs.numeric] "moduleconfigs.numeric -1"
    }

    test {apply function works} {
        catch {[r config set moduleconfigs.mutable_bool yes]} e
        assert_match {*Bool configs*} $e
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool no"
        catch {[r config set moduleconfigs.memory_numeric 1000 moduleconfigs.numeric 1000]} e
        assert_match {*cannot equal*} $e
        assert_equal [r config get moduleconfigs.memory_numeric] "moduleconfigs.memory_numeric 2097152"
        assert_equal [r config get moduleconfigs.numeric] "moduleconfigs.numeric -1"
        r module unload moduleconfigs
    }

    test {test double config argument to loadex} {
        r module loadex $testmodule CONFIG moduleconfigs.mutable_bool yes CONFIG moduleconfigs.mutable_bool no
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool no"
        r module unload moduleconfigs
    }

    test {missing loadconfigs call} {
        catch {[r module loadex $testmodule CONFIG moduleconfigs.string "cool" ARGS noload]} e
        assert_match {*ERR*} $e
    }

    test {test loadex rejects bad configs} {
        # Bad config 200gb is over the limit
        catch {[r module loadex $testmodule CONFIG moduleconfigs.memory_numeric 200gb ARGS]} e
        assert_match {*ERR*} $e
        # We should completely remove all configs on a failed load
        assert_equal [r config get moduleconfigs.*] ""
        # No value for config, should error out
        catch {[r module loadex $testmodule CONFIG moduleconfigs.mutable_bool CONFIG moduleconfigs.enum two ARGS]} e
        assert_match {*ERR*} $e
        assert_equal [r config get moduleconfigs.*] ""
        # Asan will catch this if this string is not freed
        catch {[r module loadex $testmodule CONFIG moduleconfigs.string rejectisfreed]}
        assert_match {*ERR*} $e
        assert_equal [r config get moduleconfigs.*] ""
        # test we can't set random configs
        catch {[r module loadex $testmodule CONFIG maxclients 333]}
        assert_match {*ERR*} $e
        assert_equal [r config get moduleconfigs.*] ""
        assert_not_equal [r config get maxclients] "maxclients 333"
        # test we can't set other module's configs
        r module load $testmoduletwo
        catch {[r module loadex $testmodule CONFIG configs.test no]}
        assert_match {*ERR*} $e
        assert_equal [r config get configs.test] "configs.test yes"
        r module unload configs
    }

    test {test config rewrite with dynamic load} {
        #translates to: super \0secret password
        r module loadex $testmodule CONFIG moduleconfigs.string \x73\x75\x70\x65\x72\x20\x00\x73\x65\x63\x72\x65\x74\x20\x70\x61\x73\x73\x77\x6f\x72\x64 ARGS
        assert_equal [lindex [lindex [r module list] 0] 1] moduleconfigs
        assert_equal [r config get moduleconfigs.string] "moduleconfigs.string {super \0secret password}"
        r config set moduleconfigs.mutable_bool yes
        r config set moduleconfigs.memory_numeric 750
        r config set moduleconfigs.enum two
        r config set moduleconfigs.flags "four two"
        r config rewrite
        restart_server 0 true false
        # Ensure configs we rewrote are present and that the conf file is readable
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool yes"
        assert_equal [r config get moduleconfigs.memory_numeric] "moduleconfigs.memory_numeric 750"
        assert_equal [r config get moduleconfigs.string] "moduleconfigs.string {super \0secret password}"
        assert_equal [r config get moduleconfigs.enum] "moduleconfigs.enum two"
        assert_equal [r config get moduleconfigs.flags] "moduleconfigs.flags {two four}"
        assert_equal [r config get moduleconfigs.numeric] "moduleconfigs.numeric -1"
        r module unload moduleconfigs
    }

    test {test multiple modules with configs} {
        r module load $testmodule
        r module loadex $testmoduletwo CONFIG configs.test yes
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool yes"
        assert_equal [r config get moduleconfigs.immutable_bool] "moduleconfigs.immutable_bool no"
        assert_equal [r config get moduleconfigs.memory_numeric] "moduleconfigs.memory_numeric 1024"
        assert_equal [r config get moduleconfigs.string] "moduleconfigs.string {secret password}"
        assert_equal [r config get moduleconfigs.enum] "moduleconfigs.enum one"
        assert_equal [r config get moduleconfigs.numeric] "moduleconfigs.numeric -1"
        assert_equal [r config get configs.test] "configs.test yes"
        r config set moduleconfigs.mutable_bool no
        r config set moduleconfigs.string nice
        r config set moduleconfigs.enum two
        r config set configs.test no
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool no"
        assert_equal [r config get moduleconfigs.string] "moduleconfigs.string nice"
        assert_equal [r config get moduleconfigs.enum] "moduleconfigs.enum two"
        assert_equal [r config get configs.test] "configs.test no"
        r config rewrite
        # test we can load from conf file with multiple different modules.
        restart_server 0 true false
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool no"
        assert_equal [r config get moduleconfigs.string] "moduleconfigs.string nice"
        assert_equal [r config get moduleconfigs.enum] "moduleconfigs.enum two"
        assert_equal [r config get configs.test] "configs.test no"
        r module unload moduleconfigs
        r module unload configs
    }

    test {test 1.module load 2.config rewrite 3.module unload 4.config rewrite works} {
        # Configs need to be removed from the old config file in this case.
        r module loadex $testmodule CONFIG moduleconfigs.memory_numeric 500 ARGS
        assert_equal [lindex [lindex [r module list] 0] 1] moduleconfigs
        r config rewrite
        r module unload moduleconfigs
        r config rewrite
        restart_server 0 true false
        # Ensure configs we rewrote are no longer present
        assert_equal [r config get moduleconfigs.*] ""
    }
    test {startup moduleconfigs} {
        # No loadmodule directive
        set nomodload [start_server [list overrides [list moduleconfigs.string "hello"]]]
        wait_for_condition 100 50 {
            ! [is_alive $nomodload]
        } else {
            fail "startup should've failed with no load and module configs supplied"
        }
        set stdout [dict get $nomodload stdout]
        assert_equal [count_message_lines $stdout "Module Configuration detected without loadmodule directive or no ApplyConfig call: aborting"] 1

        # Bad config value
        set badconfig [start_server [list overrides [list loadmodule "$testmodule" moduleconfigs.string "rejectisfreed"]]]
        wait_for_condition 100 50 {
            ! [is_alive $badconfig]
        } else {
            fail "startup with bad moduleconfigs should've failed"
        }
        set stdout [dict get $badconfig stdout]
        assert_equal [count_message_lines $stdout "Issue during loading of configuration moduleconfigs.string : Cannot set string to 'rejectisfreed'"] 1

        set noload [start_server [list overrides [list loadmodule "$testmodule noload" moduleconfigs.string "hello"]]]
        wait_for_condition 100 50 {
            ! [is_alive $noload]
        } else {
            fail "startup with moduleconfigs and no loadconfigs call should've failed"
        }
        set stdout [dict get $noload stdout]
        assert_equal [count_message_lines $stdout "Module Configurations were not set, likely a missing LoadConfigs call. Unloading the module."] 1

        start_server [list overrides [list loadmodule "$testmodule" moduleconfigs.string "bootedup" moduleconfigs.enum two moduleconfigs.flags "two four"]] {
            assert_equal [r config get moduleconfigs.string] "moduleconfigs.string bootedup"
            assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool yes"
            assert_equal [r config get moduleconfigs.immutable_bool] "moduleconfigs.immutable_bool no"
            assert_equal [r config get moduleconfigs.enum] "moduleconfigs.enum two"
            assert_equal [r config get moduleconfigs.flags] "moduleconfigs.flags {two four}"
            assert_equal [r config get moduleconfigs.numeric] "moduleconfigs.numeric -1"
            assert_equal [r config get moduleconfigs.memory_numeric] "moduleconfigs.memory_numeric 1024"
        }
    }
}

