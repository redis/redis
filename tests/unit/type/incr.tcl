start_server {tags {"incr"}} {
    test {INCR against non existing key} {
        set res {}
        append res [r incr novar]
        append res [r get novar]
    } {11}

    test {INCR against key created by incr itself} {
        r incr novar
    } {2}

    test {INCR against key originally set with SET} {
        r set novar 100
        r incr novar
    } {101}

    test {INCR over 32bit value} {
        r set novar 17179869184
        r incr novar
    } {17179869185}

    test {INCRBY over 32bit value with over 32bit increment} {
        r set novar 17179869184
        r incrby novar 17179869184
    } {34359738368}

    test {INCR fails against key with spaces (left)} {
        r set novar "    11"
        catch {r incr novar} err
        format $err
    } {ERR*}

    test {INCR fails against key with spaces (right)} {
        r set novar "11    "
        catch {r incr novar} err
        format $err
    } {ERR*}

    test {INCR fails against key with spaces (both)} {
        r set novar "    11    "
        catch {r incr novar} err
        format $err
    } {ERR*}

    test {DECRBY negation overflow} {
        r set x 0
        catch {r decrby x -9223372036854775808} err
        format $err
    } {ERR*}

    test {INCR fails against a key holding a list} {
        r rpush mylist 1
        catch {r incr mylist} err
        r rpop mylist
        format $err
    } {WRONGTYPE*}

    test {DECRBY over 32bit value with over 32bit increment, negative res} {
        r set novar 17179869184
        r decrby novar 17179869185
    } {-1}

    test {INCR uses shared objects in the 0-9999 range} {
        r set foo -1
        r incr foo
        assert {[r object refcount foo] > 1}
        r set foo 9998
        r incr foo
        assert {[r object refcount foo] > 1}
        r incr foo
        assert {[r object refcount foo] == 1}
    } {} {needs:debug}

    test {INCR can modify objects in-place} {
        r set foo 20000
        r incr foo
        assert {[r object refcount foo] == 1}
        set old [lindex [split [r debug object foo]] 1]
        r incr foo
        set new [lindex [split [r debug object foo]] 1]
        assert {[string range $old 0 2] eq "at:"}
        assert {[string range $new 0 2] eq "at:"}
        assert {$old eq $new}
    } {} {needs:debug}

    test {INCRBYFLOAT against non existing key} {
        r del novar
        list    [roundFloat [r incrbyfloat novar 1]] \
                [roundFloat [r get novar]] \
                [roundFloat [r incrbyfloat novar 0.25]] \
                [roundFloat [r get novar]]
    } {1 1 1.25 1.25}

    test {INCRBYFLOAT against key originally set with SET} {
        r set novar 1.5
        roundFloat [r incrbyfloat novar 1.5]
    } {3}

    test {INCRBYFLOAT over 32bit value} {
        r set novar 17179869184
        r incrbyfloat novar 1.5
    } {17179869185.5}

    test {INCRBYFLOAT over 32bit value with over 32bit increment} {
        r set novar 17179869184
        r incrbyfloat novar 17179869184
    } {34359738368}

    test {INCRBYFLOAT fails against key with spaces (left)} {
        set err {}
        r set novar "    11"
        catch {r incrbyfloat novar 1.0} err
        format $err
    } {ERR *valid*}

    test {INCRBYFLOAT fails against key with spaces (right)} {
        set err {}
        r set novar "11    "
        catch {r incrbyfloat novar 1.0} err
        format $err
    } {ERR *valid*}

    test {INCRBYFLOAT fails against key with spaces (both)} {
        set err {}
        r set novar " 11 "
        catch {r incrbyfloat novar 1.0} err
        format $err
    } {ERR *valid*}

    test {INCRBYFLOAT fails against a key holding a list} {
        r del mylist
        set err {}
        r rpush mylist 1
        catch {r incrbyfloat mylist 1.0} err
        r del mylist
        format $err
    } {WRONGTYPE*}

    # On some platforms strtold("+inf") with valgrind returns a non-inf result
    if {!$::valgrind} {
        test {INCRBYFLOAT does not allow NaN or Infinity} {
            r set foo 0
            set err {}
            catch {r incrbyfloat foo +inf} err
            set err
            # p.s. no way I can force NaN to test it from the API because
            # there is no way to increment / decrement by infinity nor to
            # perform divisions.
        } {ERR *would produce*}
    }

    test {INCRBYFLOAT decrement} {
        r set foo 1
        roundFloat [r incrbyfloat foo -1.1]
    } {-0.1}

    test {string to double with null terminator} {
        r set foo 1
        r setrange foo 2 2
        catch {r incrbyfloat foo 1} err
        format $err
    } {ERR *valid*}

    test {No negative zero} {
        r del foo
        r incrbyfloat foo [expr double(1)/41]
        r incrbyfloat foo [expr double(-1)/41]
        r get foo
    } {0}
}
