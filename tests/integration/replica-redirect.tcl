start_server {tags {needs:repl external:skip}} {
    start_server {} {
        set master_host [srv -1 host]
        set master_port [srv -1 port]

        r replicaof $master_host $master_port
        wait_for_condition 50 100 {
            [s 0 master_link_status] eq {up}
        } else {
            fail "Replicas not replicating from master"
        }

        test {replica allow read command by default} {
            r get foo
        } {}

        test {replica reply READONLY error for write command by default} {
            assert_error {READONLY*} {r set foo bar}
        }

        test {replica redirect read and write command when enable replica-enable-redirect} {
            r config set replica-enable-redirect yes
            assert_error "MOVED -1 $master_host:$master_port" {r set foo bar}
            assert_error "MOVED -1 $master_host:$master_port" {r get foo}
        }

        test {non-data access commands are not redirected} {
            r ping
        } {PONG}
    }
}
