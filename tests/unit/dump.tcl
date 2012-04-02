start_server {tags {"dump"}} {
    test {DUMP / RESTORE are able to serialize / unserialize a simple key} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        list [r exists foo] [r restore foo 0 $encoded] [r ttl foo] [r get foo]
    } {0 OK -1 bar}

    test {RESTORE can set an arbitrary expire to the materialized key} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        r restore foo 5000 $encoded
        set ttl [r pttl foo]
        assert {$ttl >= 3000 && $ttl <= 5000}
        r get foo
    } {bar}

    test {RESTORE returns an error of the key already exists} {
        r set foo bar
        set e {}
        catch {r restore foo 0 "..."} e
        set e
    } {*is busy*}

    test {DUMP of non existing key returns nil} {
        r dump nonexisting_key
    } {}

    test {MIGRATE is able to migrate a key between two instances} {
        set first [srv 0 client]
        r set key "Some Value"
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert {[$first exists key] == 1}
            assert {[$second exists key] == 0}
            set ret [r -1 migrate $second_host $second_port key 9 5000]
            assert {$ret eq {OK}}
            assert {[$first exists key] == 0}
            assert {[$second exists key] == 1}
            assert {[$second get key] eq {Some Value}}
        }
    }

    test {MIGRATE timeout actually works} {
        set first [srv 0 client]
        r set key "Some Value"
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert {[$first exists key] == 1}
            assert {[$second exists key] == 0}

            set rd [redis_deferring_client]
            $rd debug sleep 5.0 ; # Make second server unable to reply.
            set e {}
            catch {r -1 migrate $second_host $second_port key 9 1000} e
            assert_match {IOERR*} $e
        }
    }
}
