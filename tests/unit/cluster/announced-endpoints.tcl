start_cluster 2 2 {tags {external:skip cluster}} {

    test "Test change cluster-announce-port and cluster-announce-tls-port at runtime" {
        if {$::tls} {
            set baseport [lindex [R 0 config get tls-port] 1]
        } else {
            set baseport [lindex [R 0 config get port] 1]
        }
        set count [expr [llength $::servers] + 1]
        set used_port [find_available_port $baseport $count]

        R 0 config set cluster-announce-tls-port $used_port
        R 0 config set cluster-announce-port $used_port

        assert_match "*:$used_port@*" [R 0 CLUSTER NODES]
        wait_for_condition 50 100 {
            [string match "*:$used_port@*" [R 1 CLUSTER NODES]]
        } else {
            fail "Cluster announced port was not propagated via gossip"
        }

        R 0 config set cluster-announce-tls-port 0
        R 0 config set cluster-announce-port 0
        assert_match "*:$baseport@*" [R 0 CLUSTER NODES]
    }

    test "Test change cluster-announce-bus-port at runtime" {
        if {$::tls} {
            set baseport [lindex [R 0 config get tls-port] 1]
        } else {
            set baseport [lindex [R 0 config get port] 1]
        }
        set count [expr [llength $::servers] + 1]
        set used_port [find_available_port $baseport $count]

        # Verify config set cluster-announce-bus-port
        R 0 config set cluster-announce-bus-port $used_port
        assert_match "*@$used_port *" [R 0 CLUSTER NODES]
        wait_for_condition 50 100 {
            [string match "*@$used_port *" [R 1 CLUSTER NODES]]
        } else {
            fail "Cluster announced port was not propagated via gossip"
        }

        # Verify restore default cluster-announce-port
        set base_bus_port [expr $baseport + 10000]
        R 0 config set cluster-announce-bus-port 0
        assert_match "*@$base_bus_port *" [R 0 CLUSTER NODES]
    }

    test "CONFIG SET port updates cluster-announced port" {
        set count [expr [llength $::servers] + 1]
        # Get the original port and change to new_port
        if {$::tls} {
            set orig_port [lindex [R 0 config get tls-port] 1]
        } else {
            set orig_port [lindex [R 0 config get port] 1]
        }
        assert {$orig_port != ""}
        set new_port [find_available_port $orig_port $count]

        if {$::tls} {
            R 0 config set tls-port $new_port
        } else {
            R 0 config set port $new_port
        }

        # Verify that the new port appears in the output of cluster slots
        wait_for_condition 50 100 {
            [string match "*$new_port*" [R 0 cluster slots]]
        } else {
            fail "Cluster announced port was not updated in cluster slots"
        }
    }

    # Tests for cluster-announce-ip validation
    test "cluster-announce-ip validation" {
        catch {R 0 config set cluster-announce-ip "192.168.1.100\nnext"} err
        assert_match "*valid IPv4 or IPv6*" $err

        catch {R 0 config set cluster-announce-ip "10.0.0.1\ttab"} err
        assert_match "*valid IPv4 or IPv6*" $err

        catch {R 0 config set cluster-announce-ip "1.2.3.4\r\n"} err
        assert_match "*valid IPv4 or IPv6*" $err

        catch {R 0 config set cluster-announce-ip "redis-node-1.example.com"} err
        assert_match "*valid IPv4 or IPv6*" $err

        catch {R 0 config set cluster-announce-ip "192.168.1"} err
        assert_match "*valid IPv4 or IPv6*" $err

        # Accept valid IPv4
        R 0 config set cluster-announce-ip "192.168.1.100"
        assert_equal "192.168.1.100" [lindex [R 0 config get cluster-announce-ip] 1]

        # Accept valid IPv6
        R 0 config set cluster-announce-ip "2001:db8::1"
        assert_equal "2001:db8::1" [lindex [R 0 config get cluster-announce-ip] 1]

        # Can be cleared
        R 0 config set cluster-announce-ip ""
        assert_equal "" [lindex [R 0 config get cluster-announce-ip] 1]
    }

    # Tests for cluster-announce-human-nodename validation
    test "cluster-announce-human-nodename validation" {
        # Reject control characters
        catch {R 0 config set cluster-announce-human-nodename "badchar\nnext"} err
        assert_match "*invalid character*" $err

        catch {R 0 config set cluster-announce-human-nodename "bad\ttab"} err
        assert_match "*invalid character*" $err

        catch {R 0 config set cluster-announce-human-nodename "bad\r\nline"} err
        assert_match "*invalid character*" $err

        # Reject delimiter characters (comma, equals, space)
        catch {R 0 config set cluster-announce-human-nodename "bad,comma"} err
        assert_match "*invalid character*" $err

        catch {R 0 config set cluster-announce-human-nodename "bad=equals"} err
        assert_match "*invalid character*" $err

        catch {R 0 config set cluster-announce-human-nodename "bad space"} err
        assert_match "*invalid character*" $err

        # Reject quote characters (double quote, single quote, backslash)
        catch {R 0 config set cluster-announce-human-nodename "bad\"quote"} err
        assert_match "*invalid character*" $err

        catch {R 0 config set cluster-announce-human-nodename "bad'quote"} err
        assert_match "*invalid character*" $err

        catch {R 0 config set cluster-announce-human-nodename "bad\\slash"} err
        assert_match "*invalid character*" $err

        # Accept valid names
        R 0 config set cluster-announce-human-nodename "my-redis-node-1"
        assert_equal "my-redis-node-1" [lindex [R 0 config get cluster-announce-human-nodename] 1]
    }

    # DoS prevention test: verify server can restart after CLUSTER SAVECONFIG
    test "cluster-announce-ip persists correctly with CLUSTER SAVECONFIG" {
        R 0 config set cluster-announce-ip "192.168.1.100"
        R 0 cluster saveconfig

        # Verify the IP appears in CLUSTER NODES output
        assert_match "*192.168.1.100*" [R 0 cluster nodes]
    }

    test "cluster-announce-human-nodename persists correctly with CLUSTER SAVECONFIG" {
        R 0 config set cluster-announce-human-nodename "production-node-1"
        R 0 cluster saveconfig

        # Verify the nodename is set correctly
        assert_equal "production-node-1" [lindex [R 0 config get cluster-announce-human-nodename] 1]
    }
}
