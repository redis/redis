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
        r config set moduleconfigs.enum two
        assert_equal [r config get moduleconfigs.enum] "moduleconfigs.enum two"
        r config set moduleconfigs.numeric -2
        assert_equal [r config get moduleconfigs.numeric] "moduleconfigs.numeric -2"
    }

    test {Immutable flag works properly and rejected strings dont leak} {
        # Configs flagged immutable should not allow sets
        catch {[r config set moduleconfigs.immutable_bool yes]} e
        assert_match {*can't set immutable config*} $e
        catch {[r config set moduleconfigs.string rejectisfreed]} e
        assert_match {*ERR*} $e
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
        catch {[r config set moduleconfigs.enum four]} e
        assert_match {*argument must be one of the following*} $e
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
        catch {[r module loadex $testmodule CONFIG maxclients 500]}
        assert_match {*ERR*} $e
        assert_equal [r config get moduleconfigs.*] ""
        assert_equal [r config get maxclients] "maxclients 10000"
        # test we can't set other module's configs
        r module load $testmoduletwo
        catch {[r module loadex $testmodule CONFIG configs.test no]}
        assert_match {*ERR*} $e
        assert_equal [r config get configs.test] "configs.test yes"
        r module unload configs
    }

    test {test config rewrite with dynamic load} {
        r module loadex $testmodule CONFIG moduleconfigs.memory_numeric 500 ARGS
        assert_equal [lindex [lindex [r module list] 0] 1] moduleconfigs
        r config set moduleconfigs.mutable_bool yes
        r config set moduleconfigs.memory_numeric 750
        r config set moduleconfigs.enum two
        r config rewrite
        restart_server 0 true false
        # Ensure configs we rewrote are present
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool yes"
        assert_equal [r config get moduleconfigs.memory_numeric] "moduleconfigs.memory_numeric 750"
        assert_equal [r config get moduleconfigs.string] "moduleconfigs.string {secret password}"
        assert_equal [r config get moduleconfigs.enum] "moduleconfigs.enum two"
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
}

