set server_path [tmpdir "server.auth_with_acls"]

exec cp tests/assets/users-file $server_path
start_server [list overrides [list "dir" $server_path "users-file" "users-file"]] {
    # automatically default test ran with select command

    test {AUTH fails when a wrong protocol} {
        catch {r auth wrong!} err
        set _ $err
    } {ERR wrong number of arguments for 'auth(use_cmd_acl)' command}

    test {AUTH fails when a wrong id and pw} {
        catch {r auth wrongid! wrongpw!} err
        set _ $err
    } {ERR invalid user or password}

    test {run with alltest user} {
        catch {r auth alltest alltest} err
        set _ $err
    } {OK}

    test {Once AUTH succeeded we can actually send commands to the server} {
        r set foo 100
        r incr foo
    } {101}
}

exec cp tests/assets/users-file $server_path
start_server [list overrides [list "dir" $server_path "users-file" "users-file"]] {
    # automatically default test ran with select command

    test {AUTH fails when a wrong protocol} {
        catch {r auth wrong!} err
        set _ $err
    } {ERR wrong number of arguments for 'auth(use_cmd_acl)' command}

    test {AUTH fails when a wrong id and pw} {
        catch {r auth wrongid! wrongpw!} err
        set _ $err
    } {ERR invalid user or password}

    test {run with allnotping user} {
        catch {r auth allnotping allnotping} err
        set _ $err
    } {OK}

    test {Once AUTH succeeded we can actually send commands to the server} {
        r set foo 100
        r incr foo
    } {101}

    test {ACL fails with ping} {
        catch {r ping} err
        set _ $err
    } {ACL Not allowed ACL command.}
}

exec cp tests/assets/users-file $server_path
start_server [list overrides [list "dir" $server_path "users-file" "users-file"]] {
    # automatically default test ran with select command

    test {AUTH fails when a wrong protocol} {
        catch {r auth wrong!} err
        set _ $err
    } {ERR wrong number of arguments for 'auth(use_cmd_acl)' command}

    test {AUTH fails when a wrong id and pw} {
        catch {r auth wrongid! wrongpw!} err
        set _ $err
    } {ERR invalid user or password}

    test {run with allnotzset user} {
        catch {r auth allnotzset allnotzset} err
        set _ $err
    } {OK}

    test {Once AUTH succeeded we can actually send commands to the server} {
        r set foo 100
        r incr foo
    } {101}

    test {ACL fails with ping} {
        catch {r zadd ss 100 abcd} err
        set _ $err
    } {ACL Not allowed ACL command.}
}

exec cp tests/assets/users-file $server_path
start_server [list overrides [list "dir" $server_path "users-file" "users-file"]] {
    # automatically default test ran with select command

    test {AUTH fails when a wrong protocol} {
        catch {r auth wrong!} err
        set _ $err
    } {ERR wrong number of arguments for 'auth(use_cmd_acl)' command}

    test {AUTH fails when a wrong id and pw} {
        catch {r auth wrongid! wrongpw!} err
        set _ $err
    } {ERR invalid user or password}

    test {run with zsethash user} {
        catch {r auth zsethash zsethash} err
        set _ $err
    } {OK}

    test {ACL fails with set} {
        catch {r set foo 100} err
        set _ $err
    } {ACL Not allowed ACL command.}

    test {Once AUTH succeeded we can actually send commands to the server : zadd} {
        r zadd ss 100 abcd
    } {1}

    test {Once AUTH succeeded we can actually send commands to the server : hset} {
        r hset hh abcd 100
    } {1}

    test {Once AUTH succeeded we can actually send commands to the server : hget} {
        r hget hh abcd
    } {100}

    test {ACL fails with info} {
        catch {r info} err
        set _ $err
    } {ACL Not allowed ACL command.}
}

exec cp tests/assets/users-file $server_path
start_server [list overrides [list "dir" $server_path "users-file" "users-file"]] {
    # automatically default test ran with select command

    test {AUTH fails when a wrong protocol} {
        catch {r auth wrong!} err
        set _ $err
    } {ERR wrong number of arguments for 'auth(use_cmd_acl)' command}

    test {AUTH fails when a wrong id and pw} {
        catch {r auth wrongid! wrongpw!} err
        set _ $err
    } {ERR invalid user or password}

    test {run with slowtest user} {
        catch {r auth slowtest slowtest} err
        set _ $err
    } {OK}

    test {Once AUTH succeeded we can actually send commands to the server : set} {
        r set foo 100
    } {OK}

    test {ACL fails with exists} {
        catch {r info} err
        set _ $err
    } {ACL Not allowed ACL command.}
}
