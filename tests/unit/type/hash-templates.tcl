# Build a template-based hash via HIMPORT command.
proc make_hashtmpl {key args} {
    set fields {}
    set values {}
    foreach {f v} $args {
        lappend fields $f
        lappend values $v
    }
    set fsname "fieldset_[join $fields _]"
    r himport prepare $fsname {*}$fields
    r himport set $key $fsname {*}$values
}

# A key ref released by a BIO lazyfree thread (flushall etc.) is
# dropped on that background thread, so num_template_keys is eventually
# consistent: it settles once the BIO free job runs.
proc wait_num_template_keys {expected {level ""}} {
    wait_for_condition 50 100 {
        [s {*}$level hash_template_keys] == $expected
    } else {
        fail "hash_template_keys did not settle to $expected\
              (got [s {*}$level hash_template_keys])"
    }
}

# Poll hash_templates (registry size) until it settles. A template is removed
# only once its refs reach zero; the final ref may be dropped on a BIO lazyfree
# thread and reclaimed in serverCron, so the registry size is eventually 
# consistent like wait_num_template_keys above.
proc wait_num_templates {expected {level ""}} {
    wait_for_condition 50 100 {
        [s {*}$level hash_templates] == $expected
    } else {
        fail "hash_templates did not settle to $expected\
              (got [s {*}$level hash_templates])"
    }
}

# Run the full test suite under both template encodings: TMPL_LP (default
# listpack-backed) and TMPL_ARRAY (forced via hash-max-listpack-entries=0).
foreach encoding {template-listpack template-array} {
start_server {tags {"hash" "needs:debug" "cluster:skip"} overrides {hash-min-template-entries 0}} {
    if {$encoding eq "template-array"} {
        r config set hash-max-listpack-entries 0
    } else {
        r config set hash-max-listpack-entries 512
    }

    test {HIMPORT argument validation} {
        # Bad subcommand / no subcommand.
        assert_error "*wrong number of arguments*" {r himport}
        assert_error "*unknown subcommand*" {r himport foobar}

        # PREPARE needs a name and at least one field.
        assert_error "*wrong number of arguments*" {r himport prepare}
        assert_error "*wrong number of arguments*" {r himport prepare fieldset}

        # SET needs a key, a template and values.
        assert_error "*wrong number of arguments*" {r himport set}
        assert_error "*wrong number of arguments*" {r himport set k}
        assert_error "*wrong number of arguments*" {r himport set k fieldset}

        # DISCARD takes exactly one name; DISCARDALL takes none.
        assert_error "*wrong number of arguments*" {r himport discard}
        assert_error "*wrong number of arguments*" {r himport discard a b}
        assert_error "*wrong number of arguments*" {r himport discardall extra}
    }

    test {HIMPORT PREPARE with single field works} {
        assert_equal [r himport prepare fieldset f1] OK
        assert_equal [r himport set key fieldset v1] OK
        assert_equal [r hgetall key] {f1 v1}
        r himport discard fieldset
    }

    test {HIMPORT PREPARE rejects duplicate field names} {
        assert_error "*duplicate field name*" {r himport prepare fieldset1 a a b}
        assert_error "*duplicate field name*" {r himport prepare fieldset2 a b a}
        assert_error "*duplicate field name*" {r himport prepare fieldset3 same same}
        # Failed PREPARE must not register the fieldset.
        assert_error "*no such fieldset*" {r himport set key fieldset1 v1 v2 v3}
    }

    test {HIMPORT PREPARE accepts empty field name} {
        assert_equal [r himport prepare fieldset "" b] OK
        assert_equal [r himport set key fieldset va vb] OK
        assert_equal [r hget key ""] va
        assert_equal [r hget key b] vb
        r himport discard fieldset
    }

    test {HIMPORT PREPARE accepts empty fieldset name} {
        assert_equal [r himport prepare "" f1 f2] OK
        assert_equal [r himport set key "" v1 v2] OK
        assert_equal [r hgetall key] {f1 v1 f2 v2}
        r himport discard ""
    }

    test {HIMPORT PREPARE replaces existing fieldset with same name} {
        # Create template with 2 fields
        r himport prepare fieldset a b
        r himport set key1 fieldset val_a val_b
        assert_equal [r hgetall key1] {a val_a b val_b}

        # Replace with a fieldset with 3 fields (same name)
        r himport prepare fieldset x y z
        r himport set key2 fieldset val_x val_y val_z
        assert_equal [r hgetall key2] {x val_x y val_y z val_z}

        # Old fieldset definition should be gone - using with 2 values fails
        assert_error "*value count does not match*" {r himport set key3 fieldset v1 v2}

        # Cleanup
        r himport discard fieldset
    }

    test {Fieldsets are not shared between connections} {
        set rd [redis_client]
        $rd himport prepare fieldset a b
        # The main client cannot see the other connection's fieldset.
        assert_error "*no such fieldset*" {r himport set key fieldset v1 v2}
        # The fieldset still works on its owning connection.
        assert_equal [$rd himport set key fieldset v1 v2] OK
        $rd close
        r del key
    }

    test {Multiple keys share the same template} {
        # Start from an empty registry so the counts below are exact.
        r flushall
        r himport discardall
        wait_num_templates 0

        r himport prepare shared name email age
        r himport set shared:1 shared alice alice@example.com 25
        r himport set shared:2 shared bob bob@example.com 30
        r himport set shared:3 shared charlie charlie@example.com 35

        assert_encoding $encoding shared:1
        assert_encoding $encoding shared:2
        assert_encoding $encoding shared:3

        # Three keys, but a single template in the registry
        assert_equal [s hash_templates] 1
        assert_equal [s hash_template_keys] 3

        assert_equal [r hget shared:1 name] alice
        assert_equal [r hget shared:2 name] bob
        assert_equal [r hget shared:3 name] charlie
    }

    test {Different field orders use same template} {
        # Start from an empty registry so the counts below are exact.
        r flushall
        r himport discardall
        wait_num_templates 0

        # Same fields in different orders must resolve to one shared template.
        r himport prepare order1 a b c
        r himport prepare order2 c b a
        r himport prepare order3 b a c

        # Create hashes - values follow each fieldset's declared order.
        r himport set order:key1 order1 va1 vb1 vc1
        r himport set order:key2 order2 vc2 vb2 va2
        r himport set order:key3 order3 vb3 va3 vc3

        assert_encoding $encoding order:key1
        assert_encoding $encoding order:key2
        assert_encoding $encoding order:key3

        # Three distinct fieldsets and three keys, yet one template:
        assert_equal [s hash_templates] 1
        assert_equal [s hash_template_keys] 3

        # All should have same sorted field order: a, b, c
        assert_equal [r hget order:key1 a] va1
        assert_equal [r hget order:key2 a] va2
        assert_equal [r hget order:key3 a] va3

        assert_equal [r hget order:key1 b] vb1
        assert_equal [r hget order:key2 b] vb2
        assert_equal [r hget order:key3 b] vb3
    }

    test {Template is released only after its last reference is dropped} {
        # Start from an empty registry so the counts below are exact.
        r flushall
        r himport discardall
        wait_num_templates 0

        # One template, pinned by a fieldset and two keys.
        r himport prepare fs x y z
        r himport set rc:1 fs 1 2 3
        r himport set rc:2 fs 4 5 6
        assert_equal [s hash_templates] 1
        assert_equal [s hash_template_keys] 2

        # Deleting every key drops the key-refs to zero, but the still prepared
        # fieldset keeps the template alive
        r del rc:1 rc:2
        assert_equal [s hash_template_keys] 0
        assert_equal [s hash_templates] 1
        r himport set rc:3 fs 7 8 9
        assert_equal [s hash_template_keys] 1
        assert_equal [s hash_templates] 1

        # Drop the last key and the fieldset, template will be deleted
        r del rc:3
        assert_equal [r himport discard fs] 1
        wait_num_templates 0
        assert_equal [s hash_template_keys] 0
    }

    test {HIMPORT SET with too many values fails} {
        r himport prepare fieldset a b
        assert_error "*value count does not match*" {r himport set key fieldset v1 v2 v3}
        r himport discard fieldset
    }

    test {HIMPORT SET with too few values fails} {
        r himport prepare fieldset a b c
        assert_error "*value count does not match*" {r himport set key fieldset v1 v2}
        r himport discard fieldset
    }

    test {HIMPORT DISCARD on nonexistent fieldset returns 0} {
        assert_equal [r himport discard does_not_exist] 0
    }

    test {HIMPORT DISCARD returns 1 when fieldset removed} {
        r himport prepare fieldset a b
        assert_equal [r himport discard fieldset] 1
        assert_equal [r himport discard fieldset] 0
    }

    test {HIMPORT DISCARD does not invalidate existing keys} {
        r himport prepare fieldset a b c
        r himport set key1 fieldset v1 v2 v3
        assert_encoding $encoding key1
        # Discard the fieldset; existing key must remain valid.
        r himport discard fieldset
        assert_encoding $encoding key1
        assert_equal [r hgetall key1] {a v1 b v2 c v3}
        # SET with the discarded name now fails.
        assert_error "*no such fieldset*" {r himport set key2 fieldset v1 v2 v3}
        r del key1
    }

    test {HIMPORT DISCARDALL with no fieldsets returns 0} {
        r himport discardall
        assert_equal [r himport discardall] 0
    }

    test {HIMPORT DISCARDALL returns number of removed fieldsets} {
        r himport discardall
        r himport prepare fieldset1 x y
        r himport prepare fieldset2 x y z
        r himport prepare fieldset3 m n
        assert_equal [r himport discardall] 3
        foreach name {fieldset1 fieldset2 fieldset3} {
            assert_error "*no such fieldset*" {r himport set k $name v1 v2}
        }
    }

    test {HIMPORT SET with unknown fieldset fails} {
        r del myhash
        assert_error "*no such fieldset*" {r himport set myhash nosuchfs alice alice@example.com 25}
    }

    test {HIMPORT SET refuses to overwrite a non-hash key (WRONGTYPE)} {
        r del myhash
        r himport prepare user name email age
        r set myhash "string value"
        assert_error "WRONGTYPE*" {r himport set myhash user charlie charlie@example.com 30}
        assert_equal [r type myhash] {string}
        assert_equal [r get myhash] {string value}
    }

    test {HIMPORT SET replaces existing regular hash} {
        r del myhash
        r himport prepare user name email age
        r hset myhash oldfield oldvalue
        r himport set myhash user dave dave@example.com 40
        assert_encoding $encoding myhash
        assert_equal [r hget myhash name] {dave}
        assert_equal [r hget myhash oldfield] {}
    }

    test {HIMPORT SET replaces existing template-based hash} {
        r del myhash
        r himport prepare user name email age
        r himport set myhash user eve eve@example.com 25
        assert_encoding $encoding myhash
        # Replace the same key using a different fieldset.
        r himport prepare user2 city country
        r himport set myhash user2 Paris France
        assert_encoding $encoding myhash
        assert_equal [r hget myhash city] {Paris}
        assert_equal [r hget myhash name] {}
    }

    test {HIMPORT PREPARE state is accounted in client memory} {
        # tot-mem from CLIENT INFO for the current connection.
        proc cur_tot_mem {} {
            regexp {tot-mem=(\d+)} [r client info] -> m
            return $m
        }

        r himport discardall
        set before [cur_tot_mem]

        # Prepare many large fieldsets; each one owns the fieldset and pins a template
        for {set i 0} {$i < 100} {incr i} {
            set fields {}
            for {set f 0} {$f < 64} {incr f} {
                lappend fields "field_${i}_${f}"
            }
            r himport prepare fieldset$i {*}$fields
        }
        set after [cur_tot_mem]

        # Client-owned fieldset state must be visible in tot-mem.
        assert {$after > $before}

        # Releasing the fieldsets returns the accounted memory.
        r himport discardall
        set discarded [cur_tot_mem]
        assert {$discarded < $after}
    }

    test {HIMPORT PREPARE/DISCARD accounting does not drift} {
        proc prepare_100 {} {
            for {set i 0} {$i < 100} {incr i} {
                set fields {}
                for {set f 0} {$f < 64} {incr f} { lappend fields "field_${i}_${f}" }
                r himport prepare fieldset$i {*}$fields
            }
        }
        # Measure how much memory 100 fieldsets add.
        r himport discardall
        set empty [cur_tot_mem]
        prepare_100
        set overhead [expr {[cur_tot_mem] - $empty}]
        r himport discardall

        # After clearing, memory must return near $empty. tot-mem has some buffer
        # noise, so allow half a cycle of overhead; a leak would add much more.
        set tol [expr {$overhead / 2}]
        for {set cycle 0} {$cycle < 4} {incr cycle} {
            prepare_100
            # Clear with DISCARDALL on even cycles, one-by-one DISCARD on odd ones.
            if {$cycle % 2} {
                for {set i 0} {$i < 100} {incr i} { r himport discard fieldset$i }
            } else {
                r himport discardall
            }
            assert {abs([cur_tot_mem] - $empty) < $tol}
        }
    }

    test {HIMPORT PREPARE state can trigger maxmemory-clients eviction} {
        # Returns the CLIENT LIST entry for $name, or "" if not connected.
        proc himport_client_line {name} {
            set clients [split [string trim [r client list]] "\r\n"]
            return [lsearch -inline $clients *name=$name*]
        }

        set saved_limit [lindex [r config get maxmemory-clients] 1]
        r config set maxmemory-clients 2mb
        r client no-evict on ;# protect the main test connection

        # A separate connection that accumulates HIMPORT fieldset state. A
        # single PREPARE's query buffer (~tens of KB) stays far below the 2mb
        # limit, so eviction can only fire once the accounted fieldset memory
        # (value_order maps) adds up past it.
        set rr [redis_client]
        $rr client setname himport_client
        assert {[himport_client_line himport_client] ne ""}

        set evicted_before [s evicted_clients]
        set evicted 0
        for {set i 0} {$i < 4000} {incr i} {
            set fields {}
            for {set f 0} {$f < 2000} {incr f} {
                lappend fields "field_${i}_${f}"
            }
            if {[catch {$rr himport prepare fieldset$i {*}$fields}]} {
                set evicted 1 ;# server closed the connection on eviction
                break
            }
            if {[himport_client_line himport_client] eq ""} {
                set evicted 1 ;# evicted asynchronously in beforeSleep
                break
            }
        }
        assert {$evicted}
        # Confirm it was a client eviction (not some other disconnect).
        assert {[s evicted_clients] > $evicted_before}

        catch {$rr close}
        r config set maxmemory-clients $saved_limit
    }

    test {CLIENT RESET clears session-local fieldsets} {
        r himport prepare fieldset a b c
        assert_equal [r himport set key1 fieldset v1 v2 v3] OK
        r reset
        assert_error "*no such fieldset*" {r himport set key2 fieldset v1 v2 v3}
        r del key1
    }

    test {Client disconnect frees its session-local fieldsets} {
        # Preparing a fieldset with a new field set adds one template to the
        # registry. Disconnecting the client that owns it must free its
        # fieldsets, so the template (referenced by no key) is removed again.
        set base_template_count [s hash_templates]
        set rd [redis_client]
        $rd himport prepare fieldset f1 f2 f3
        assert_equal [expr {$base_template_count + 1}] [s hash_templates]
        $rd close
        # The registry settles back once the disconnect is processed.
        wait_num_templates $base_template_count
    }

    test {EVAL invocations do not share session-local fieldsets} {
        assert_equal [r eval {redis.call('HIMPORT','PREPARE','fieldset','a'); return 'OK'} 0] OK
        assert_error "*no such fieldset*" {r eval {return redis.call('HIMPORT','SET','key','fieldset','1')} 0}

        assert_equal [r eval {
            redis.call('HIMPORT','PREPARE','fieldset','a')
            return redis.call('HIMPORT','SET','key','fieldset','1')
        } 0] OK
        assert_equal [r hgetall key] {a 1}
        r del key
    }

    test {FCALL invocations do not share session-local fieldsets} {
        r function flush
        r function load replace {#!lua name=himporttest
            redis.register_function('prepare_only', function(KEYS, ARGV)
                return redis.call('HIMPORT','PREPARE','fieldset','a')
            end)
            redis.register_function('set_only', function(KEYS, ARGV)
                return redis.call('HIMPORT','SET',KEYS[1],'fieldset','1')
            end)
            redis.register_function('prepare_and_set', function(KEYS, ARGV)
                redis.call('HIMPORT','PREPARE','fieldset','a')
                return redis.call('HIMPORT','SET',KEYS[1],'fieldset','1')
            end)
        }

        assert_equal [r fcall prepare_only 0] OK
        assert_error "*no such fieldset*" {r fcall set_only 1 key}

        assert_equal [r fcall prepare_and_set 1 key] OK
        assert_equal [r hgetall key] {a 1}
        r del key
        r function flush
    }

    # --- MULTI / EXEC ---

    test {HIMPORT PREPARE/SET inside MULTI/EXEC works} {
        r multi
        r himport prepare fieldset a b c
        r himport set key fieldset v1 v2 v3
        set replies [r exec]
        assert_equal [lindex $replies 0] OK
        assert_equal [lindex $replies 1] OK
        assert_encoding $encoding key
        assert_equal [r hgetall key] {a v1 b v2 c v3}
        r himport discard fieldset
    }

    test {HIMPORT DISCARDALL inside MULTI/EXEC works} {
        r himport prepare fieldset1 a b
        r himport prepare fieldset2 c d
        r multi
        r himport discardall
        r exec
        assert_error "*no such fieldset*" {r himport set k fieldset1 v1 v2}
    }

    test {HGET on template-based hash} {
        make_hashtmpl basic:test name alice email alice@example.com age 25
        assert_equal [r hget basic:test name] {alice}
        assert_equal [r hget basic:test email] {alice@example.com}
        assert_equal [r hget basic:test age] {25}
        assert_equal [r hget basic:test nonexistent] {}
    }

    test {HGETALL on template-based hash} {
        make_hashtmpl basic:getall f1 v1 f2 v2 3 3
        assert_equal {3 3 f1 v1 f2 v2} [r hgetall basic:getall]
    }

    test {HLEN on template-based hash} {
        make_hashtmpl basic:len a 1 b 2 c 3 d 4
        assert_equal [r hlen basic:len] 4
    }

    test {HEXISTS on template-based hash} {
        make_hashtmpl basic:exists name alice
        assert_equal [r hexists basic:exists name] 1
        assert_equal [r hexists basic:exists nonexistent] 0
    }

    test {HINCRBY on template-based hash} {
        make_hashtmpl basic:incr counter 10
        assert_equal [r hincrby basic:incr counter 5] 15
        assert_equal [r hget basic:incr counter] 15
        assert_encoding $encoding basic:incr
    }

    test {HINCRBYFLOAT on template-based hash} {
        make_hashtmpl basic:incrfloat value 10.5
        set result [r hincrbyfloat basic:incrfloat value 0.1]
        assert_range $result 10.5 10.7
        assert_encoding $encoding basic:incrfloat
    }

    test {HMGET on template-based hash} {
        make_hashtmpl basic:hmget a 1 b 2 c 3
        assert_equal [r hmget basic:hmget a c] {1 3}
        assert_equal [r hmget basic:hmget a nonexistent c] {1 {} 3}
    }

    test {HKEYS on template-based hash} {
        make_hashtmpl basic:keys name alice email bob
        set keys [r hkeys basic:keys]
        assert_equal [lsort $keys] {email name}
    }

    test {HVALS on template-based hash} {
        make_hashtmpl basic:vals name alice email bob
        set vals [r hvals basic:vals]
        assert_equal [lsort $vals] {alice bob}
    }

    test {HSTRLEN on template-based hash} {
        make_hashtmpl basic:strlen name alice
        assert_equal [r hstrlen basic:strlen name] 5
    }

    test {HDEL on template-based hash keeps template encoding} {
        make_hashtmpl hdel:test a 1 b 2 c 3 d 4
        assert_encoding $encoding hdel:test
        assert_equal [r hdel hdel:test b] 1
        assert_encoding $encoding hdel:test
        assert_equal [r hgetall hdel:test] {a 1 c 3 d 4}
    }

    test {HDEL multiple fields on template-based hash} {
        make_hashtmpl hdel:multi a 1 b 2 c 3 d 4 e 5
        assert_equal [r hdel hdel:multi b d] 2
        assert_encoding $encoding hdel:multi
        assert_equal [r hgetall hdel:multi] {a 1 c 3 e 5}
    }

    test {HDEL all fields deletes the key} {
        make_hashtmpl hdel:all a 1 b 2
        r hdel hdel:all a b
        assert_equal [r exists hdel:all] 0
    }

    test {HGETDEL returns value and keeps template encoding} {
        make_hashtmpl hgetdel:test a 1 b 2 c 3 d 4
        assert_encoding $encoding hgetdel:test
        assert_equal [r hgetdel hgetdel:test FIELDS 1 b] {2}
        assert_encoding $encoding hgetdel:test
        assert_equal [r hgetall hgetdel:test] {a 1 c 3 d 4}
    }

    test {HGETDEL multiple fields on template-based hash} {
        make_hashtmpl hgetdel:multi a 1 b 2 c 3 d 4 e 5
        assert_equal [r hgetdel hgetdel:multi FIELDS 2 b d] {2 4}
        assert_encoding $encoding hgetdel:multi
        assert_equal [r hgetall hgetdel:multi] {a 1 c 3 e 5}
    }

    test {HGETDEL non-existent field returns nil and keeps encoding} {
        make_hashtmpl hgetdel:miss a 1 b 2
        set res [r hgetdel hgetdel:miss FIELDS 1 zzz]
        assert_equal [lindex $res 0] {}
        assert_encoding $encoding hgetdel:miss
        assert_equal [r hlen hgetdel:miss] 2
    }

    test {HGETDEL all fields deletes the key} {
        make_hashtmpl hgetdel:all a 1 b 2
        assert_equal [r hgetdel hgetdel:all FIELDS 2 a b] {1 2}
        assert_equal [r exists hgetdel:all] 0
    }

    test {HGETEX without options returns values and keeps template encoding} {
        make_hashtmpl hgetex:plain a 1 b 2 c 3
        assert_encoding $encoding hgetex:plain
        assert_equal [r hgetex hgetex:plain FIELDS 2 a c] {1 3}
        assert_encoding $encoding hgetex:plain
    }

    test {HGETEX non-existent field returns nil and keeps encoding} {
        make_hashtmpl hgetex:miss a 1 b 2
        set res [r hgetex hgetex:miss FIELDS 1 zzz]
        assert_equal [lindex $res 0] {}
        assert_encoding $encoding hgetex:miss
    }

    test {HGETEX PERSIST returns value and leaves field without TTL} {
        make_hashtmpl hgetex:persist a 1 b 2
        assert_encoding $encoding hgetex:persist
        # PERSIST on a field that has no TTL is a no-op. Whether the hash
        # deconverts from the template is an implementation detail we don't
        # assert on; what matters is the values survive and no TTL is left.
        assert_equal [r hgetex hgetex:persist PERSIST FIELDS 1 a] {1}
        assert_equal [lindex [r httl hgetex:persist FIELDS 1 a] 0] -1
        assert_equal [r hget hgetex:persist a] 1
        assert_equal [r hget hgetex:persist b] 2
    }

    # HGETEX EX attaches a field TTL, so it deconverts to a TTL-capable encoding
    # like HEXPIRE: listpackex under the LP iter, hashtable under the AR iter
    # (entries=0).
    test {HGETEX EX converts template to a TTL-capable encoding and sets TTL} {
        set ex_target [expr {$encoding eq "template-listpack" ? "listpackex" : "hashtable"}]
        make_hashtmpl hgetex:ex name alice email alice@example.com
        assert_encoding $encoding hgetex:ex
        assert_equal [r hgetex hgetex:ex EX 100 FIELDS 1 name] {alice}
        assert_encoding $ex_target hgetex:ex
        set ttl [lindex [r httl hgetex:ex FIELDS 1 name] 0]
        assert_range $ttl 1 100
        assert_equal [r hget hgetex:ex email] alice@example.com
    }

    test {HSET adds new field to template-based hash} {
        make_hashtmpl hset:add name alice email alice@example.com
        assert_encoding $encoding hset:add
        r hset hset:add age 25
        assert_encoding $encoding hset:add
        assert_equal [r hlen hset:add] 3
        assert_equal [r hget hset:add age] 25
    }

    test {HSET updates existing field in template-based hash} {
        make_hashtmpl hset:update name alice
        r hset hset:update name bob
        assert_encoding $encoding hset:update
        assert_equal [r hget hset:update name] bob
    }

    test {HSET multiple fields on template-based hash} {
        make_hashtmpl hset:multi a 1
        r hset hset:multi b 2 c 3 d 4
        assert_encoding $encoding hset:multi
        assert_equal {a 1 b 2 c 3 d 4} [r hgetall hset:multi]
    }

    test {HSETNX on existing field in template-based hash} {
        make_hashtmpl hsetnx:test name alice
        assert_equal [r hsetnx hsetnx:test name bob] 0
        assert_equal [r hget hsetnx:test name] alice
    }

    test {HSETNX on new field in template-based hash} {
        make_hashtmpl hsetnx:new name alice
        assert_equal [r hsetnx hsetnx:new email alice@example.com] 1
        assert_encoding $encoding hsetnx:new
        assert_equal [r hget hsetnx:new email] alice@example.com
    }

    # HSCAN on template-based hash
    # The template iterator walks fields in template (sorted) order, so HSCAN
    # output is deterministic and can be compared verbatim. Each case below
    # covers a distinct branch of the TMPL_LP/TMPL_ARRAY scan path in db.c, and
    # the whole file runs under both encodings via the outer foreach.

    # No pattern, with values: integer values.
    test {HSCAN on template-based hash returns all field/value pairs} {
        make_hashtmpl key a 1 b 2 c 3
        assert_encoding $encoding key
        set result [r hscan key 0]
        assert_equal [lindex $result 0] 0
        assert_equal [lindex $result 1] {a 1 b 2 c 3}
    }

    # No pattern, with values: string values.
    test {HSCAN on template-based hash returns string values verbatim} {
        make_hashtmpl key f1 val1 f2 val2 f3 val3
        set result [r hscan key 0]
        assert_equal [lindex $result 1] {f1 val1 f2 val2 f3 val3}
    }

    # no_values branch: reply length is n, values skipped.
    test {HSCAN NOVALUES on template-based hash returns fields only} {
        make_hashtmpl key a 1 b 2 c 3
        set result [r hscan key 0 NOVALUES]
        assert_equal [lindex $result 0] 0
        assert_equal [lindex $result 1] {a b c}
    }

    # use_pattern branch with a partial match.
    test {HSCAN MATCH on template-based hash selects a subset} {
        make_hashtmpl key field1 val1 field2 val2 other val3
        set result [r hscan key 0 MATCH field*]
        assert_equal [lindex $result 0] 0
        assert_equal [lindex $result 1] {field1 val1 field2 val2}
    }

    # use_pattern where every field hits the continue path: empty result, cursor 0.
    test {HSCAN MATCH on template-based hash matching nothing is empty} {
        make_hashtmpl key a 1 b 2 c 3
        set result [r hscan key 0 MATCH nomatch*]
        assert_equal [lindex $result 0] 0
        assert_equal [lindex $result 1] {}
    }

    # use_pattern where every field matches: full result through the deferred path.
    test {HSCAN MATCH * on template-based hash returns everything} {
        make_hashtmpl key a 1 b 2 c 3
        set result [r hscan key 0 MATCH *]
        assert_equal [lindex $result 1] {a 1 b 2 c 3}
    }

    # use_pattern and no_values together: matched fields only, no values.
    test {HSCAN MATCH with NOVALUES on template-based hash} {
        make_hashtmpl key field1 val1 field2 val2 other val3
        set result [r hscan key 0 MATCH field* NOVALUES]
        assert_equal [lindex $result 0] 0
        assert_equal [lindex $result 1] {field1 field2}
    }

    # COUNT is ignored on this path (single-shot scan); cursor stays 0.
    test {HSCAN COUNT is ignored on template-based hash} {
        make_hashtmpl key a 1 b 2 c 3 d 4 e 5
        set result [r hscan key 0 COUNT 1]
        assert_equal [lindex $result 0] 0
        assert_equal [lindex $result 1] {a 1 b 2 c 3 d 4 e 5}
    }

    # Single-field hash edge case.
    test {HSCAN on single-field template-based hash} {
        make_hashtmpl key only val
        set result [r hscan key 0]
        assert_equal [lindex $result 0] 0
        assert_equal [lindex $result 1] {only val}
    }

    test {HRANDFIELD on template-based hash} {
        make_hashtmpl hrand:test a 1 b 2 c 3
        set field [r hrandfield hrand:test]
        assert {$field eq "a" || $field eq "b" || $field eq "c"}
    }

    test {HRANDFIELD with count on template-based hash} {
        make_hashtmpl hrand:count a 1 b 2 c 3 d 4
        set fields [r hrandfield hrand:count 2]
        assert_equal [llength $fields] 2
        # Positive count returns distinct, real fields of the hash.
        assert_equal [llength [lsort -unique $fields]] 2
        foreach f $fields { assert {$f in {a b c d}} }
    }

    test {HRANDFIELD with WITHVALUES on template-based hash} {
        make_hashtmpl hrand:withval a 1 b 2
        set result [r hrandfield hrand:withval 2 WITHVALUES]
        assert_equal [llength $result] 4
        # Each returned field must be paired with its own value.
        foreach {f v} $result { assert_equal $v [dict get {a 1 b 2} $f] }
    }

    test {HRANDFIELD WITHVALUES RESP3 reply on template-based hash} {
        make_hashtmpl hrand:resp3 a 1 b 2 c 3
        r hello 3
        set res [r hrandfield hrand:resp3 3 withvalues]
        assert_equal [llength $res] 3
        assert_equal [llength [lindex $res 0]] 2
        # RESP3 nests each field/value as a pair; verify the pairing.
        foreach pair $res { lassign $pair f v; assert_equal $v [dict get {a 1 b 2 c 3} $f] }
        r hello 2
    }

    test {HRANDFIELD negative count on template-based hash} {
        make_hashtmpl hrand:neg a 1 b 2
        set result [r hrandfield hrand:neg -5]
        assert_equal [llength $result] 5
        # Negative count samples with replacement; every draw is a real field.
        foreach f $result { assert {$f in {a b}} }
    }

    test {HRANDFIELD negative count WITHVALUES on template-based hash} {
        make_hashtmpl hrand:nwv a 1 b vb c 3
        assert_encoding $encoding hrand:nwv
        set res [r hrandfield hrand:nwv -6 WITHVALUES]
        assert_equal [llength $res] 12
        foreach {f v} $res { assert_equal $v [dict get {a 1 b vb c 3} $f] }
    }

    test {HRANDFIELD negative count WITHVALUES on template-based hash (RESP3)} {
        make_hashtmpl hrand:nwv3 a 1 b vb
        r hello 3
        set res [r hrandfield hrand:nwv3 -4 WITHVALUES]
        assert_equal [llength $res] 4
        foreach pair $res { lassign $pair f v; assert_equal $v [dict get {a 1 b vb} $f] }
        r hello 2
    }

    # Hash Field Expiration - converts template keys to regular hash keys.
    # Applying a field TTL must deconvert the template-encoded hash to a
    # TTL-capable encoding. The target depends on hash-max-listpack-entries:
    # "listpackex" under the LP iter, "hashtable" under the AR iter (entries=0).
    set hfe_target [expr {$encoding eq "template-listpack" ? "listpackex" : "hashtable"}]

    test {HPEXPIRE on template-based hash converts away from template} {
        make_hashtmpl hfe:test name alice email alice@example.com
        assert_encoding $encoding hfe:test
        assert_equal [r hpexpire hfe:test 100000 FIELDS 1 name] {1}
        assert_encoding $hfe_target hfe:test

        # Field data is preserved across the conversion.
        assert_equal [r hget hfe:test name] alice
        assert_equal [r hget hfe:test email] alice@example.com

        # The TTL is set.
        set ttl [lindex [r hpttl hfe:test FIELDS 1 name] 0]
        assert_range $ttl 1 100000
        assert_equal [r hpttl hfe:test FIELDS 1 email] {-1}
    }

    test {HEXPIRE on template-based hash converts away from template} {
        make_hashtmpl hfe:expire name bob age 30
        assert_encoding $encoding hfe:expire
        assert_equal [r hexpire hfe:expire 100 FIELDS 1 age] {1}
        assert_encoding $hfe_target hfe:expire

        # Field data is preserved and the TTL is set
        assert_equal [r hget hfe:expire age] 30
        assert_equal [r hget hfe:expire name] bob

        set ttl [lindex [r httl hfe:expire FIELDS 1 age] 0]
        assert_range $ttl 1 100
        assert_equal [r httl hfe:expire FIELDS 1 name] {-1}
    }

    test {HSETEX without expiration keeps template encoding} {
        make_hashtmpl hfe:noexp name alice
        # HSETEX with no expiration token only sets fields, so the hash must
        # stay template-encoded just like a plain HSET.
        assert_equal [r hsetex hfe:noexp FIELDS 1 email alice@example.com] 1
        assert_encoding $encoding hfe:noexp
        assert_equal [r hget hfe:noexp email] alice@example.com
    }

    test {HSETEX with expiration converts template key to regular hash} {
        make_hashtmpl hfe:setex name alice email alice@example.com
        assert_encoding $encoding hfe:setex

        # An expiration token forces the template hash into an HFE-capable
        # encoding, exactly like HEXPIRE/HPEXPIRE above.
        assert_equal [r hsetex hfe:setex EX 100 FIELDS 1 name bob] 1
        assert_encoding $hfe_target hfe:setex

        # The set value is visible and the TTL is active only on that field.
        assert_equal [r hget hfe:setex name] bob
        set ttl [lindex [r httl hfe:setex FIELDS 1 name] 0]
        assert_range $ttl 1 100
        assert_equal [r httl hfe:setex FIELDS 1 email] {-1}
    }

    # Read-only HFE commands on a template hash reply:
    #   - existing field -> -1 (no TTL),
    #   - missing field -> -2 (no such field).
    test {read-only HFE commands on template hash} {
        make_hashtmpl hfe:ro a 1 b 2
        assert_encoding $encoding hfe:ro
        foreach cmd {httl hpttl hexpiretime hpexpiretime hpersist} {
            assert_equal [r $cmd hfe:ro FIELDS 2 a missing_field] {-1 -2}
        }
        assert_encoding $encoding hfe:ro
    }

    test {HFE on template-based hash actually expires the field} {
        make_hashtmpl hfe:gone name alice email bob
        assert_encoding $encoding hfe:gone

        # A short TTL deconverts the template and must really expire the field.
        set expired_before [s expired_subkeys]
        assert_equal [r hpexpire hfe:gone 50 FIELDS 1 name] {1}
        wait_for_condition 50 20 {
            [r hexists hfe:gone name] == 0
        } else {
            fail "field 'name' did not expire on template-based hash"
        }
        # The field went away through the HFE expiry path, not some other delete.
        assert {[s expired_subkeys] > $expired_before}
        # The untouched field keeps its value after the expiry.
        assert_equal [r hget hfe:gone email] bob
    }

    test {INFO stats shows hash template stats} {
        r flushall
        wait_num_template_keys 0

        # Create 3 keys with same template
        make_hashtmpl info:k1 a 1 b 2
        make_hashtmpl info:k2 a 3 b 4
        make_hashtmpl info:k3 a 5 b 6
        set keys1 [s hash_template_keys]
        assert_equal $keys1 3

        # Create 1 key with different template
        make_hashtmpl info:k4 x 1 y 2 z 3
        set keys2 [s hash_template_keys]
        assert_equal $keys2 4

        # Delete one key
        r del info:k1
        set keys3 [s hash_template_keys]
        assert_equal $keys3 3

        # Flushall resets to 0
        r flushall
        wait_num_template_keys 0
    }

    test {template registry stays usable after emptying and refilling} {
        # Drain to an empty registry: delete all keys and discard every fieldset
        # prepared by earlier tests on this connection, so no template is left.
        r flushall
        r himport discardall
        wait_num_templates 0

        make_hashtmpl shrink:k a 1 b 2 c 3
        assert {[s hash_templates] >= 1}

        # Remove the key and the fieldset so the registry empties again.
        r del shrink:k
        r himport discardall
        wait_num_templates 0

        # After the registry has dropped back to empty, creating a fresh template
        # must still work (it has to regrow from nothing).
        make_hashtmpl shrink:k2 a 1 b 2 c 3
        assert_encoding $encoding shrink:k2
        assert_equal [r hget shrink:k2 a] 1
        assert {[s hash_templates] == 1}

        r del shrink:k2
        r himport discardall
    }

    test {FLUSHALL with template-based hashes does not crash} {
        make_hashtmpl flush:test1 a 1 b 2 c 3
        make_hashtmpl flush:test2 a 4 b 5 c 6
        make_hashtmpl flush:test3 x 1 y 2
        assert_encoding $encoding flush:test1
        r flushall
        assert_equal [r dbsize] 0

        # Create new hashes after flush
        make_hashtmpl flush:new a 1 b 2
        assert_encoding $encoding flush:new
        assert_equal [r hget flush:new a] 1
    }

    test {FLUSHDB with template-based hashes does not crash} {
        r select 1
        make_hashtmpl flushdb:test a 1 b 2
        assert_encoding $encoding flushdb:test
        r flushdb
        assert_equal [r dbsize] 0
        r select 0
    }
    
    test {Multiple FLUSHALL ASYNC with keys in succession does not crash} {
        make_hashtmpl multi:flush a 1 b 2
        r flushall async
        make_hashtmpl multi:flush1 a 1 b 2
        r flushall async
        make_hashtmpl multi:flush2 a 1 b 2
        r flushall async
        make_hashtmpl multi:flush3 a 1 b 2 c 3
        r flushall async

        r himport discardall
        wait_num_template_keys 0
        wait_num_templates 0 0
        r ping
    } {PONG}

    test {EXPIRE on template-based hash works} {
        make_hashtmpl expire:test name alice
        r expire expire:test 100
        set ttl [r ttl expire:test]
        assert_range $ttl 1 100
        assert_encoding $encoding expire:test
    }

    test {COPY template-based hash to new key} {
        make_hashtmpl copy:src name alice email bob
        assert_encoding $encoding copy:src
        r copy copy:src copy:dst
        assert_encoding $encoding copy:dst
        assert_equal [r hgetall copy:dst] [r hgetall copy:src]
    }

    test {COPY template-based hash with REPLACE} {
        make_hashtmpl copy:replace:src a 1 b 2
        r set copy:replace:dst "string"
        r copy copy:replace:src copy:replace:dst REPLACE
        assert_equal [r type copy:replace:dst] hash
        assert_encoding $encoding copy:replace:dst
    }
}

# RDB save/load
start_server {tags {"hash" "rdb" "needs:debug" "cluster:skip"}
              overrides {hash-min-template-entries 0}} {
    if {$encoding eq "template-array"} {
        r config set hash-max-listpack-entries 0
    }

    test {RDB save and load preserves template-based hash} {
        make_hashtmpl rdb:test name alice email alice@example.com age 25
        assert_encoding $encoding rdb:test

        r debug reload

        assert_encoding $encoding rdb:test
        # Length-first sort: age (3) < name (4) < email (5)
        assert_equal [r hgetall rdb:test] {age 25 name alice email alice@example.com}
    }

    test {RDB save and load multiple template-based hashes with shared template} {
        r flushall
        r himport discardall
        wait_num_templates 0
        make_hashtmpl rdb:multi1 a 1 b 2 c 3
        make_hashtmpl rdb:multi2 a 4 b 5 c 6
        make_hashtmpl rdb:multi3 x 1 y 2

        r debug reload

        assert_encoding $encoding rdb:multi1
        assert_encoding $encoding rdb:multi2
        assert_encoding $encoding rdb:multi3
        assert_equal [r hgetall rdb:multi1] {a 1 b 2 c 3}
        assert_equal [r hgetall rdb:multi2] {a 4 b 5 c 6}
        assert_equal [r hgetall rdb:multi3] {x 1 y 2}

        wait_num_templates 2
        wait_num_template_keys 3
    }

    test {RDB save stores template hashes in compact REF form} {
        # An RDB stores each template's field names once (REF form), not once per
        # key. To prove it, save N keys sharing one template twice: first with
        # short field names, then with long ones. Per-key storage would grow the
        # RDB by N copies of the extra name bytes; REF form grows by just one copy,
        # so the size barely moves.
        #
        # rdbcompression off so LZF can't hide the repeated names.
        r config set rdbcompression no
        set rdb_path [file join [lindex [r config get dir] 1] \
                                [lindex [r config get dbfilename] 1]]
        set n 1000
        set values {v1 v2 v3 v4 v5 v6 v7 v8}

        # N keys sharing one template, short (2-char) field names.
        r flushall
        r himport prepare fs_short f1 f2 f3 f4 f5 f6 f7 f8
        for {set i 0} {$i < $n} {incr i} { r himport set short:$i fs_short {*}$values }
        assert_encoding $encoding short:0
        r save
        set size_short [file size $rdb_path]

        # Same N keys, same values, but each field name ~48 bytes longer.
        set long_fields {}
        foreach f {f1 f2 f3 f4 f5 f6 f7 f8} { lappend long_fields $f[string repeat _ 48] }
        r flushall
        r himport prepare fs_long {*}$long_fields
        for {set i 0} {$i < $n} {incr i} { r himport set long:$i fs_long {*}$values }
        assert_encoding $encoding long:0
        r save
        set size_long [file size $rdb_path]

        # Long names add 8 * 48 = 384 bytes. Stored once that is the whole growth;
        # stored per key it would be N * 384 = ~384 KB. The threshold sits well
        # between the two, so this fails loudly if names are ever stored per key.
        assert {$size_long - $size_short < 50000}
    }

    if {$encoding eq "template-listpack"} {
    test {Loading a TMPL_LP may convert it to TMPL_ARRAY due to hash-max-listpack-entries} {
        r flushall
        set saved [lindex [r config get hash-max-listpack-entries] 1]
        r config set hash-max-listpack-entries 128

        # 40-field hash fits the 128 limit -> TMPL_LP.
        set fields {}
        set vals {}
        for {set i 0} {$i < 40} {incr i} {
            lappend fields [format f%03d $i]
            lappend vals v$i
        }
        r himport prepare wide {*}$fields
        r himport set wk wide {*}$vals
        assert_encoding template-listpack wk
        set dump [r dump wk]

        # Shrink the limit below the field count. Both load paths must convert
        # TMPL_LP to TMPL_ARRAY:
        r config set hash-max-listpack-entries 8

        # RDB load
        r debug reload
        assert_encoding template-array wk
        assert_equal [r hget wk f000] v0
        assert_equal [r hget wk f039] v39
        assert_equal [r hlen wk] 40

        # RESTORE
        r restore wk2 0 $dump
        assert_encoding template-array wk2
        assert_equal [r hget wk2 f000] v0
        assert_equal [r hget wk2 f039] v39
        assert_equal [r hlen wk2] 40

        r config set hash-max-listpack-entries $saved
        r himport discardall
        r flushall
        wait_num_template_keys 0
        wait_num_templates 0 0
    }
    }
}

# AOF rewrite tests
start_server {tags {"hash" "needs:debug" "cluster:skip" "external:skip"}
              overrides {save {} appendonly yes auto-aof-rewrite-percentage 0
                         appendfsync always hash-min-template-entries 0}} {

    foreach rdbpre {yes no} {
        test "AOF rewrite preserves template and plain hashes (rdb-preamble=$rdbpre)" {
            r config set aof-use-rdb-preamble $rdbpre
            if {$encoding eq "template-array"} { r config set hash-max-listpack-entries 0 }
            r flushall
            wait_num_template_keys 0

            r bgrewriteaof
            waitForBgrewriteaof r
            set aof [get_last_incr_aof_path r]

            make_hashtmpl aof:k1 a 1 b 2 c 3
            make_hashtmpl aof:k2 a 4 b 5 c 6
            make_hashtmpl aof:k3 x 7 y 8
            # Plain hashes alongside the template ones must round-trip unchanged.
            r hset aof:plain1 x 100 y 200
            r hset aof:plain2 m 1 n 2 o 3

            # HIMPORT SET propagates to the AOF as RESTORE RESPLACE; 
            # plain HSET stays HSET.
            assert_aof_content $aof {
                {select *}
                {restore aof:k1 0 * REPLACE}
                {restore aof:k2 0 * REPLACE}
                {restore aof:k3 0 * REPLACE}
                {hset aof:plain1 x 100 y 200}
                {hset aof:plain2 m 1 n 2 o 3}
            }

            assert_encoding $encoding aof:k1
            set tmpls_before [s hash_templates]
            set keys_before [s hash_template_keys]

            r bgrewriteaof
            waitForBgrewriteaof r
            r debug loadaof

            assert_encoding $encoding aof:k1
            assert_encoding $encoding aof:k2
            assert_encoding $encoding aof:k3

            set plain_enc [expr {$encoding eq "template-array" ? "hashtable" : "listpack"}]
            assert_equal [r object encoding aof:plain1] $plain_enc
            assert_equal [r object encoding aof:plain2] $plain_enc

            assert_equal $tmpls_before [s hash_templates]
            wait_num_template_keys $keys_before

            assert_equal [r hgetall aof:k1] {a 1 b 2 c 3}
            assert_equal [r hgetall aof:k2] {a 4 b 5 c 6}
            assert_equal [r hgetall aof:k3] {x 7 y 8}
        }
    }
}

# Replication tests
start_server {tags {"hash" "repl" "needs:repl" "needs:debug" "cluster:skip" "external:skip"}
              overrides {hash-min-template-entries 0}} {
    start_server {overrides {hash-min-template-entries 0}} {
        test {HIMPORT SET: hashes replicate via RDB and replication stream} {
            set master [srv -1 client]
            set master_host [srv -1 host]
            set master_port [srv -1 port]
            set replica [srv 0 client]

            # A >64-byte value forces the template-array encoding (small values stay
            # template-listpack). fs_short's field names fit a listpack; fs_long has
            # a >64-byte field name that does not. Each fieldset gets a small-value
            # and a large-value key, so the four keys cover every combination of
            # value encoding and field-name length.
            set big [string repeat x 100]
            $master himport prepare fs_short f1 f2 f3
            $master himport prepare fs_long  f1 f2 [string repeat q 70]

            # Phase 1: create all four keys BEFORE the replica syncs, so they travel
            # inside the full-sync RDB.
            $master himport set rdb:lp_short_fields  fs_short v1 v2 v3
            $master himport set rdb:arr_short_fields fs_short v1 $big v3
            $master himport set rdb:lp_long_fields   fs_long  v1 v2 v3
            $master himport set rdb:arr_long_fields  fs_long  v1 v2 $big

            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq "up"
            } else {
                fail "Replica did not sync"
            }
            # The full-sync RDB carried all four.
            assert_equal 1 [$replica exists rdb:arr_long_fields]

            # Phase 2: create the same four keys AFTER sync; each HIMPORT SET must
            # propagate as a self-contained RESTORE on the replication stream.
            set repl [attach_to_replication_stream_on_connection -1]
            $master himport set str:lp_short_fields  fs_short v1 v2 v3
            $master himport set str:arr_short_fields fs_short v1 $big v3
            $master himport set str:lp_long_fields   fs_long  v1 v2 v3
            $master himport set str:arr_long_fields  fs_long  v1 v2 $big
            assert_replication_stream $repl {
                {select *}
                {restore str:lp_short_fields 0 * REPLACE}
                {restore str:arr_short_fields 0 * REPLACE}
                {restore str:lp_long_fields 0 * REPLACE}
                {restore str:arr_long_fields 0 * REPLACE}
            }
            close_replication_stream $repl

            wait_for_condition 50 100 {
                [$replica exists str:arr_long_fields] == 1
            } else {
                fail "stream key not replicated"
            }

            # Encodings survived on the replica for every key.
            foreach k {rdb:lp_short_fields rdb:lp_long_fields str:lp_short_fields str:lp_long_fields} {
                assert_equal template-listpack [$replica object encoding $k]
            }
            foreach k {rdb:arr_short_fields rdb:arr_long_fields str:arr_short_fields str:arr_long_fields} {
                assert_equal template-array [$replica object encoding $k]
            }

            # All 8 keys are identical in content on master and replica.
            assert_equal [$master debug digest] [$replica debug digest]

            # Deleting keys removes the replica's only references to the templates
            foreach k {rdb:lp_short_fields rdb:arr_short_fields rdb:lp_long_fields rdb:arr_long_fields
                       str:lp_short_fields str:arr_short_fields str:lp_long_fields str:arr_long_fields} {
                $master del $k
            }
            wait_num_templates 0 0
        }
    }
}

# Two back-to-back full resyncs must not leak the load-time template registry:
# templates seen during one load must not linger into the next (else the second
# load re-adds the same templates and fails). Exercised over both sync paths.
proc assert_resync_keeps_templates {encoding} {
    set master [srv -1 client]
    set master_host [srv -1 host]
    set master_port [srv -1 port]
    set replica [srv 0 client]

    if {$encoding eq "template-array"} { $master config set hash-max-listpack-entries 0 }
    $master flushall

    # 1000 keys, each backed by its own distinct 3-field template.
    for {set i 0} {$i < 1000} {incr i} {
        $master himport prepare fs$i f${i}_a f${i}_b f${i}_c
        $master himport set dr:$i fs$i 1 2 3
    }
    assert_equal [$master object encoding dr:0] $encoding
    assert_equal [s -1 hash_templates] 1000
    assert_equal [s -1 hash_template_keys] 1000
    set digest [$master debug digest]

    # Full resync #1: the replica reloads all 1000 templates + keys, byte-identical.
    $replica replicaof $master_host $master_port
    wait_for_sync $replica
    assert_equal [$replica debug digest] $digest
    assert_equal [s 0 hash_templates] 1000
    wait_num_template_keys 1000 0

    # Full resync #2 (changing the master replid forces a full sync) reloads the
    # registry a second time; the first load's templates must not have leaked.
    $replica replicaof no one
    $master debug change-repl-id
    $replica replicaof $master_host $master_port
    wait_for_sync $replica

    assert_equal [$replica debug digest] $digest
    assert_equal [s 0 hash_templates] 1000
    wait_num_template_keys 1000 0
    assert {[s -1 sync_full] >= 2}
}

# Exercise both sync paths: diskless and disk based.
foreach {kind diskless_sync diskless_load} {
    diskless yes swapdb
    disk     no  disabled
} {
    start_server {tags {"hash" "repl" "needs:repl" "needs:debug" "cluster:skip" "external:skip"}
                  overrides {hash-min-template-entries 0}} {
        start_server {overrides {hash-min-template-entries 0}} {
            test "$kind replica survives repeated full resyncs with templates ($encoding)" {
                [srv -1 client] config set repl-diskless-sync $diskless_sync
                [srv -1 client] config set repl-diskless-sync-delay 0
                [srv 0 client] config set repl-diskless-load $diskless_load
                assert_resync_keeps_templates $encoding
            }
        }
    }
}
} ;# end foreach encoding

# HRANDFIELD batch optimization tests 
start_server {tags {"hash" "cluster:skip"}} {
    test {HRANDFIELD large count batching on TMPL_LP} {
        # Test batch random sampling optimization (avoid O(count*n) lpSeek overhead).
        r config set hash-min-template-entries 50

        r del h1 h2
        set fields {}
        for {set i 0} {$i < 100} {incr i} {
            lappend fields "f$i" "v$i"
        }
        # Two hashes with same fields -> triggers template
        r hset h1 {*}$fields
        r hset h2 {*}$fields

        set enc [r object encoding h1]
        assert_equal template-listpack $enc

        # Large count with duplicates (negative count)
        set result [r hrandfield h1 -500]
        assert_equal [llength $result] 500

        foreach field $result {
            assert_match "f*" $field
        }

        # With WITHVALUES: each field "fN" must pair with its own value "vN"
        set result [r hrandfield h1 -300 WITHVALUES]
        assert_equal [llength $result] 600
        for {set i 0} {$i < 600} {incr i 2} {
            set f [lindex $result $i]
            set v [lindex $result [expr {$i + 1}]]
            assert_match "f*" $f
            assert_equal "v[string range $f 1 end]" $v
        }
    }

    test {HRANDFIELD positive count WITHVALUES pairs correctly on TMPL_LP} {
        # Positive count exercises the unique-index path (CASE 2.5b), which also
        # collects value pointers in a single pass instead of per-draw lpSeek.
        r config set hash-min-template-entries 50

        r del h1 h2
        set fields {}
        for {set i 0} {$i < 100} {incr i} {
            lappend fields "f$i" "v$i"
        }
        r hset h1 {*}$fields
        r hset h2 {*}$fields
        assert_equal template-listpack [r object encoding h1]

        # 40 unique fields, each paired with its own value, no duplicates.
        set result [r hrandfield h1 40 WITHVALUES]
        assert_equal [llength $result] 80
        set seen {}
        for {set i 0} {$i < 80} {incr i 2} {
            set f [lindex $result $i]
            set v [lindex $result [expr {$i + 1}]]
            assert_equal "v[string range $f 1 end]" $v
            assert {[lsearch -exact $seen $f] == -1}
            lappend seen $f
        }
    }

    test {HRANDFIELD large count on TMPL_ARRAY} {
        r config set hash-min-template-entries 50

        r del big1 big2
        set fields {}
        # Use 600 fields to definitely exceed listpack limit
        for {set i 0} {$i < 600} {incr i} {
            lappend fields "field$i" "value$i"
        }
        r hset big1 {*}$fields
        r hset big2 {*}$fields

        set enc [r object encoding big1]
        assert_equal template-array $enc

        # TMPL_ARRAY uses simple loop (O(1) array access)
        set result [r hrandfield big1 -1000]
        assert_equal [llength $result] 1000

        foreach field $result {
            assert_match "field*" $field
        }
    }
}

# Serializing a template (DUMP) caches its field names; that cache counts toward
# used_memory_hash_templates and is freed by a cron once the template is idle.
start_server {tags {"hash" "needs:debug" "cluster:skip"}
              overrides {hash-min-template-entries 0}} {
    test "template serialization memory is accounted, then reclaimed when idle" {
        r flushall
        r himport discardall
        wait_num_templates 0

        r config set hash-max-listpack-entries 512
        set fields {}
        set vals {}
        for {set i 0} {$i < 250} {incr i} {
            lappend fields "f${i}[string repeat _ 55]" ;# ~60 bytes each -> ~15 KB
            lappend vals v
        }
        r himport prepare fs {*}$fields
        r himport set k fs {*}$vals

        r dump k  ;# serializing builds the ~15 KB cache
        set mem_with_cache [s used_memory_hash_templates]

        # Idle: the cron frees the cache and the accounted total drops back. Require
        # a >10 KB drop so this passes only if the cache was counted and then freed.
        wait_for_condition 100 100 {
            $mem_with_cache - [s used_memory_hash_templates] > 10240
        } else {
            fail "serialization cache ($mem_with_cache bytes) was not reclaimed"
        }
    }
}

# Tests under hash-min-template-entries=1: plain HSET-alike commands auto-convert
# to a template encoding.
foreach encoding {template-listpack template-array} {
start_server {tags {"hash" "needs:debug" "cluster:skip"}
              overrides {hash-min-template-entries 1}} {
    if {$encoding eq "template-array"} { r config set hash-max-listpack-entries 0 }

    test "HSET auto-converts to template encoding ($encoding)" {
        r flushall
        wait_num_templates 0

        assert_equal 0 [s hash_templates]
        assert_equal 0 [s hash_template_keys]
        r hset key1 a 1 b 2 c 3
        assert_equal 1 [s hash_templates]
        assert_equal 1 [s hash_template_keys]
        assert_equal [r object encoding key1] $encoding
        assert_equal [r hgetall key1] {a 1 b 2 c 3}
    }

    test "same field set shares a single template ($encoding)" {
        r flushall
        wait_num_templates 0
        r hset key1 a 1 b 2 c 3
        set t1 [s hash_templates]
        r hset key2 a 9 b 8 c 7
        r hset key3 a 0 b 0 c 0
        assert_equal $t1 [s hash_templates]
        assert_equal [s hash_template_keys] 3
        assert_equal [r hget key2 b] 8
    }

    test "HFE prevents template conversion ($encoding)" {
        r flushall
        wait_num_templates 0
        # HFE forces the hash away from the template encoding: a small hash lands
        # on listpackex, one too big for a listpack (array config here) on hashtable.
        set hfe_enc [expr {$encoding eq "template-array" ? "hashtable" : "listpackex"}]
        r hset hfe:1 a 1 b 2 c 3
        r hexpire hfe:1 100 FIELDS 1 a
        assert_equal [r object encoding hfe:1] $hfe_enc
        # New hash created with HFE from the start: never a template.
        r hsetex hfe:2 EX 100 FIELDS 1 a 1
        assert_equal [r object encoding hfe:2] $hfe_enc
    }

    test "RDB save/load preserves template-converted hashes ($encoding)" {
        r flushall
        wait_num_templates 0
        r hset rdb:k1 a 1 b 2 c 3
        r hset rdb:k2 a 9 b 8 c 7
        assert_equal 1 [s hash_templates]
        assert_equal 2 [s hash_template_keys]

        r debug reload
        assert_equal 1 [s hash_templates]
        assert_equal 2 [s hash_template_keys]

        assert_equal [r hgetall rdb:k1] {a 1 b 2 c 3}
        assert_equal [r hget rdb:k2 b] 8
        assert_equal [r object encoding rdb:k1] $encoding
    }

    test "DEL releases template ref when key is dropped ($encoding)" {
        r flushall
        wait_num_templates 0
        r hset del:1 a 1 b 2 c 3
        set k1 [s hash_template_keys]
        r del del:1
        set k2 [s hash_template_keys]
        assert_equal [expr {$k1 - $k2}] 1
    }

    test "AOF rewrite preserves auto-converted hashes ($encoding)" {
        r config set aof-use-rdb-preamble no
        r flushall
        wait_num_templates 0
        waitForBgrewriteaof r
        r hset aof:1 a 1 b 2 c 3
        r hset aof:2 a 9 b 8 c 7
        assert_equal 1 [s hash_templates]
        assert_equal 2 [s hash_template_keys]
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal 1 [s hash_templates]
        assert_equal 2 [s hash_template_keys]
        assert_equal [r hgetall aof:1] {a 1 b 2 c 3}
        assert_equal [r hgetall aof:2] {a 9 b 8 c 7}
        assert_equal [r object encoding aof:1] $encoding
        assert_equal [r object encoding aof:2] $encoding
    }

    test "every convert-triggering write path auto-converts ($encoding)" {
        r flushall
        wait_num_templates 0
        # Each command creates a new single field key that crosses the
        # threshold; every write path that triggers conversion must end up
        # template-encoded (HSET is covered separately above).
        foreach {name cmd} {
            hsetnx       {hsetnx conv:hsetnx f v}
            hsetex       {hsetex conv:hsetex FIELDS 1 f v}
            hincrby      {hincrby conv:hincrby f 5}
            hincrbyfloat {hincrbyfloat conv:hincrbyfloat f 1.5}
        } {
            r {*}$cmd
            set key [lindex $cmd 1]
            assert_equal [r object encoding $key] $encoding \
                "$name should auto-convert $key to $encoding"
        }
    }
}
}

# ============================================================
# Conversion bounds: a hash auto-converts only when its field count is within
# [hash-min-template-entries, hash-max-template-entries]; below min or above max
# it stays plain.
# ============================================================
foreach encoding {template-listpack template-array} {
start_server {tags {"hash" "needs:debug" "cluster:skip"}
              overrides {hash-min-template-entries 3 hash-max-template-entries 5}} {
    if {$encoding eq "template-array"} { r config set hash-max-listpack-entries 0 }

    # Plain (non-template) encoding of a small hash under the current config.
    set plain_enc [expr {$encoding eq "template-array" ? "hashtable" : "listpack"}]

    test "bound: hash within min..max auto-converts ($encoding)" {
        r flushall
        wait_num_templates 0
        # 4 fields: strictly inside [3,5] -> auto-converts.
        r hset mid a 1 b 2 c 3 d 4
        assert_equal [r object encoding mid] $encoding
        assert_equal [s hash_templates] 1
    }

    test "bound: hashes below min stay plain ($encoding)" {
        r flushall
        wait_num_templates 0
        # 1 and 2 fields: below the min (3) -> stay plain, no template created.
        r hset one a 1
        r hset two a 1 b 2
        assert_encoding $plain_enc one
        assert_encoding $plain_enc two
        assert_equal 0 [s hash_templates]
    }

    test "bound: growing a hash up to min converts it in place ($encoding)" {
        r flushall
        wait_num_templates 0
        r hset k a 1 b 2       ;# 2 fields -> below min, still plain
        assert_encoding $plain_enc k
        assert_equal 0 [s hash_templates]
        r hset k c 3           ;# 3rd field reaches min -> converts
        assert_equal [r object encoding k] $encoding
        assert_equal 1 [s hash_templates]
    }

    test "bound: hash above max stays plain ($encoding)" {
        r flushall
        wait_num_templates 0
        r hset wide f0 v0 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
        assert_encoding $plain_enc wide
        assert_equal [s hash_templates] 0
        # Same via HSETEX: the >max field count must keep it plain on this write path too.
        r hsetex wide_ex FIELDS 6 f0 v0 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
        assert_encoding $plain_enc wide_ex
        assert_equal [s hash_templates] 0
    }

    test "bound: max=0 disables the upper bound ($encoding)" {
        r flushall
        wait_num_templates 0
        r config set hash-max-template-entries 0
        r hset wide2 f0 v0 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
        assert_equal [r object encoding wide2] $encoding
        r config set hash-max-template-entries 5
    }
}
}

# Encoding conversion path coverage.
# Each test exercises one specific runtime conversion path
# between TMPL_LP / TMPL_AR / LISTPACK / LISTPACK_EX / HT.
start_server {tags {"hash" "convert" "needs:debug" "cluster:skip"}
              overrides {hash-min-template-entries 0
                         hash-max-listpack-entries 8
                         hash-max-listpack-value 64}} {

    test {convert: TMPL_LP -> TMPL_ARRAY via large value on an existing field} {
        set big [string repeat x 100]
        r himport prepare fieldset a b c d
        r himport set key fieldset 1 2 3 4
        assert_encoding template-listpack key

        r hset key a $big
        assert_encoding template-array key
        assert_equal [r hget key a] $big
        assert_equal [r hget key d] 4

        r del key
        r himport discard fieldset
    }

    test {convert: TMPL_LP -> TMPL_ARRAY via large value on a new field} {
        set big [string repeat x 100]
        r himport prepare fieldset a b c d
        r himport set key fieldset 1 2 3 4
        assert_encoding template-listpack key

        r hset key e $big
        assert_encoding template-array key
        assert_equal [r hlen key] 5
        assert_equal [r hget key e] $big
        assert_equal [r hget key a] 1

        r del key
        r himport discard fieldset
    }

    test {convert: TMPL_LP -> TMPL_ARRAY via HSET new fields (count > listpack-entries)} {
        # hash-max-listpack-entries is 8. Start at the limit.
        set fields {}; set values {}
        for {set i 0} {$i < 8} {incr i} { lappend fields f$i; lappend values v$i }
        r himport prepare fieldset {*}$fields
        r himport set key fieldset {*}$values
        assert_encoding template-listpack key

        # The 9th field pushes past the limit -> TMPL_ARRAY.
        r hset key f8 v8
        assert_encoding template-array key
        assert_equal [r hlen key] 9
        for {set i 0} {$i < 9} {incr i} { assert_equal [r hget key f$i] v$i }

        r del key
        r himport discard fieldset
    }

    test {convert: TMPL_LP -> LISTPACK_EX via HEXPIRE} {
        r himport prepare fieldset a b c d
        r himport set key fieldset 1 2 3 4
        assert_encoding template-listpack key

        r hexpire key 100 FIELDS 1 a
        assert_encoding listpackex key
        assert_equal [r hget key b] 2
        # The TTL was actually applied to field a.
        assert_range [lindex [r httl key FIELDS 1 a] 0] 90 100
        assert_equal [r httl key FIELDS 1 b] -1

        r del key
        r himport discard fieldset
    }

    test {convert: TMPL_LP -> HT via HEXPIRE (count > listpack-entries)} {
        set prev_e [lindex [r config get hash-max-listpack-entries] 1]
        r config set hash-max-listpack-entries 32

        set fields {}; set values {}
        for {set i 0} {$i < 16} {incr i} {
            lappend fields "f$i"; lappend values "v$i"
        }
        r himport prepare fieldset {*}$fields
        r himport set key fieldset {*}$values
        assert_equal [r object encoding key] template-listpack

        # Tighten limit so it converts LP_EX -> HT.
        r config set hash-max-listpack-entries 4
        r hexpire key 100 FIELDS 1 f0
        assert_equal [r object encoding key] hashtable
        assert_equal [r hget key f5] v5

        r del key
        r himport discard fieldset
        r config set hash-max-listpack-entries $prev_e
    }

    test {convert: TMPL_ARRAY -> LISTPACK_EX via HEXPIRE (small)} {
        set prev_e [lindex [r config get hash-max-listpack-entries] 1]
        # Force TMPL_ARRAY at create time.
        r config set hash-max-listpack-entries 0
        r himport prepare fieldset a b c d
        r himport set key fieldset 1 2 3 4
        r config set hash-max-listpack-entries $prev_e
        assert_equal [r object encoding key] template-array

        r hexpire key 100 FIELDS 1 a
        assert_equal [r object encoding key] listpackex
        assert_equal [r hget key b] 2

        r del key
        r himport discard fieldset
    }

    test {convert: TMPL_ARRAY -> HT via HEXPIRE (count > listpack-entries)} {
        set fields {}; set values {}
        for {set i 0} {$i < 16} {incr i} { lappend fields f$i; lappend values v$i }
        r himport prepare fieldset {*}$fields
        r himport set key fieldset {*}$values
        assert_encoding template-array key

        r hexpire key 100 FIELDS 1 f0
        assert_encoding hashtable key
        assert_equal [r hget key f7] v7

        r del key
        r himport discard fieldset
    }
}

# Memory savings. Many identical hashes that share one template must be far
# cheaper as templates, because the field names are stored once in the shared
# template instead of once per key. Each test creates 20000 identical hashes in
# both forms and requires the template form to be meaningfully smaller, both for
# a single key (MEMORY USAGE) and across all keys (used_memory).
start_server {tags {"hash" "memory" "needs:debug" "cluster:skip"}
              overrides {hash-min-template-entries 0}} {
    # Every hash holds the same 32 fields. Field names and values are the same
    # length, so in a plain hash exactly half the payload is field-name bytes,
    # which is the part a template stores once instead of once per key.
    set field_names {}
    set field_values {}
    set hset_pairs {}
    for {set i 0} {$i < 32} {incr i} {
        set name  [format "shared_field_name_%02d" $i]
        set value [format "shared_value_num_%03d" $i]
        lappend field_names  $name
        lappend field_values $value
        lappend hset_pairs   $name $value
    }

    foreach {plain_enc tmpl_enc lp_entries} {
        listpack  template-listpack 128
        hashtable template-array     0
    } {
        test "template $tmpl_enc uses less memory than plain $plain_enc" {
            r flushall
            r config set hash-max-listpack-entries $lp_entries

            # 20000 plain hashes; record total growth and one key's size.
            set mem_before [s used_memory]
            set rd [redis_deferring_client]
            deferred_batch $rd 20000 { $rd hset plain:$i {*}$hset_pairs }
            $rd close
            assert_equal [r object encoding plain:0] $plain_enc
            set plain_total  [expr {[s used_memory] - $mem_before}]
            set plain_single [r memory usage plain:0]

            r flushall

            # 20000 template hashes sharing one template (PREPARE once, same connection).
            set mem_before [s used_memory]
            set rd [redis_deferring_client]
            $rd himport prepare template {*}$field_names; $rd read
            deferred_batch $rd 20000 { $rd himport set tmpl:$i template {*}$field_values }
            $rd close
            assert_equal [r object encoding tmpl:0] $tmpl_enc
            assert_equal [r hget tmpl:0 shared_field_name_00] shared_value_num_000
            set tmpl_total  [expr {[s used_memory] - $mem_before}]
            set tmpl_single [r memory usage tmpl:0]

            assert {$tmpl_single <= $plain_single * 0.7}
            assert {$tmpl_total  <= $plain_total  * 0.7}
        }
    }

    test {INFO and MEMORY STATS track shared template memory} {
        r flushall
        wait_num_templates 0
        set base_info  [s used_memory_hash_templates]
        set base_stats [dict get [r memory stats] hash.templates]

        # Two distinct schemas -> two templates in the registry.
        r himport prepare schemaA fa1 fa2 fa3
        r himport set keyA schemaA 1 2 3
        r himport prepare schemaB fb1 fb2 fb3 fb4
        r himport set keyB schemaB 1 2 3 4
        assert_equal 2 [s hash_templates]

        # Both numbers must rise above the empty baseline and agree.
        set with_info  [s used_memory_hash_templates]
        set with_stats [dict get [r memory stats] hash.templates]
        assert {$with_info  > $base_info}
        assert {$with_stats > $base_stats}
        assert_equal $with_info $with_stats

        # Drop key refs. The counter falls back to the baseline.
        r himport discardall
        r flushall
        wait_for_condition 50 100 {
            [s hash_templates] == 0 &&
            [s used_memory_hash_templates] <= $base_info
        } else {
            fail "template memory did not return to baseline after flush\
                  (templates=[s hash_templates] mem=[s used_memory_hash_templates] base=$base_info)"
        }
        assert_equal $base_info [dict get [r memory stats] hash.templates]
    }

    test {MEMORY USAGE attributes template share per key} {
        r flushall
        wait_num_templates 0

        # Long field names so the shared template cost is non-trivial.
        set fields {}
        set vals {}
        for {set i 0} {$i < 32} {incr i} {
            lappend fields [format "a_pretty_long_shared_field_name_%03d" $i]
            lappend vals v$i
        }
        set rd [redis_deferring_client]
        $rd himport prepare tmpl {*}$fields; $rd read

        # Only one key references the template: it carries the full share.
        $rd himport set solo tmpl {*}$vals; $rd read
        set usage_solo [r memory usage solo]

        # Many more keys share the same template: the per-key share shrinks.
        deferred_batch $rd 200 { $rd himport set shared:$i tmpl {*}$vals }
        $rd close

        set usage_shared [r memory usage solo]
        assert {$usage_shared < $usage_solo}
    }
}

# Stress freeing template hashes concurrently with async flushing to surface any
# use-after-free in the cleanup path. Probabilistic guard for ASan/TSan.
start_server {tags {"hash" "needs:debug" "cluster:skip" "external:skip"}
              overrides {hash-min-template-entries 0
                         lazyfree-lazy-user-flush yes}} {
    test {template lifecycle fuzzer: BIO free races main-thread drop} {
        # Unique field names per round so templates fully release on the flush.
        for {set round 0} {$round < 100} {incr round} {
            for {set i 0} {$i < 10} {incr i} {
                if {$i % 3 == 0} {
                    set s {}
                    for {set f 0} {$f < 40} {incr f} { lappend s r${round}_${i}_$f }
                    set vals {}
                    foreach f $s { lappend vals [string repeat v 80] }
                } else {
                    set s [list r${round}_${i}_a r${round}_${i}_b r${round}_${i}_c]
                    set vals {x y z}
                }
                r himport prepare fieldset$i {*}$s
                r himport set k:$round:$i fieldset$i {*}$vals
            }
            r flushall async
            for {set p 0} {$p < 5} {incr p} { r ping }
            r himport discardall
        }
        r flushall
        wait_for_condition 200 50 { [s hash_templates] == 0 } else {
            fail "templates did not drain: [s hash_templates]"
        }
        assert_equal PONG [r ping]
    }

    test {Attach key to a template and delete its last key concurrently} {
        r flushall
        r config set hash-min-template-entries 1
        r config set lazyfree-lazy-user-del yes
        wait_num_templates 0

        r hset k1 a 1 b 2 c 3
        assert_equal 1 [s hash_templates]

        r del k1                ;# lazy delete -> BIO queues the template for free
        r hset k1 a 4 b 5 c 6   ;# recreate with the same field set (resurrection)
        after 150               ;# let the hash template cron run to collect dropped key ref

        # Still present: the drain skipped the re-referenced id.
        assert_equal 1 [s hash_templates]
        assert_equal 1 [s hash_template_keys]
        assert_equal 5 [r hget k1 b]
    }
}

# Load-time conversion of plain hashes is governed by hash-rdb-load-min-template-entries
start_server {tags {"hash" "needs:debug" "cluster:skip" "external:skip"}
              overrides {hash-min-template-entries 0 hash-max-listpack-entries 64 appendonly no}} {
    # Negative control: with hash-rdb-load-min-template-entries at its default
    # (0 = off), an RDB load must create NO templates.
    test {load-time conversion off (default): RDB load creates no templates} {
        r flushall
        assert_equal 0 [lindex [r config get hash-rdb-load-min-template-entries] 1]
        for {set i 0} {$i < 5}   {incr i} { r hset small f$i v$i }
        for {set i 0} {$i < 200} {incr i} { r hset big f$i v$i }
        assert_equal listpack  [r object encoding small]
        assert_equal hashtable [r object encoding big]
        r save
        restart_server 0 true false
        assert_equal 0 [s hash_templates]   ;# the guard: nothing was converted on load
        assert_equal listpack  [r object encoding small]
        assert_equal hashtable [r object encoding big]
        assert_equal v3   [r hget small f3]
        assert_equal v100 [r hget big f100]
    }

    test {hash-rdb-load-min-template-entries>0: restart converts listpack to TMPL_LP and hashtable to TMPL_ARRAY} {
        r flushall
        for {set i 0} {$i < 5}   {incr i} { r hset small f$i v$i }
        for {set i 0} {$i < 200} {incr i} { r hset big f$i v$i }
        assert_equal 0 [s hash_templates]

        # Persist the config so the restarted server converts on load.
        r config set hash-rdb-load-min-template-entries 4
        r config rewrite
        r save
        restart_server 0 true false
        
        # Both plain hashes were converted at load time
        assert_equal template-listpack [r object encoding small]
        assert_equal template-array    [r object encoding big]
        assert_equal 2 [s hash_templates]
        assert_equal v3   [r hget small f3]
        assert_equal v100 [r hget big f100]
    }

    test {RESTORE ignores hash-rdb-load configs} {
        # The rdb-load conversion configs apply only to bulk RDB load (the
        # restart test above), never to RESTORE. A plain dumped hash must come
        # back plain even with the rdb-load config enabled.
        r config set hash-min-template-entries 0
        r config set hash-rdb-load-min-template-entries 0
        r flushall
        wait_num_templates 0
        
        for {set i 0} {$i < 5}   {incr i} { r hset src_lp f$i v$i }
        for {set i 0} {$i < 200} {incr i} { r hset src_ht f$i v$i }
        assert_equal listpack  [r object encoding src_lp]
        assert_equal hashtable [r object encoding src_ht]
        
        set lp_payload [r dump src_lp]
        set ht_payload [r dump src_ht]
        
        # Enable load-time conversion: RESTORE must still ignore it.
        r config set hash-rdb-load-min-template-entries 4
        r restore dst_lp 0 $lp_payload
        r restore dst_ht 0 $ht_payload
        
        assert_equal listpack  [r object encoding dst_lp]
        assert_equal hashtable [r object encoding dst_ht]
        assert_equal 0 [s hash_templates]
        assert_equal v3   [r hget dst_lp f3]
        assert_equal v100 [r hget dst_ht f100]
    }
}

# hash-rdb-load-template-disassembly-threshold: when an RDB with no templates is
# loaded with load-time conversion on, a converted template is kept only if it
# ends up shared by at least this many keys; templates below the threshold are
# disassembled back to plain hashes at the end of the load.
start_server {tags {"hash" "needs:debug" "cluster:skip" "external:skip"}
              overrides {hash-min-template-entries 0 hash-max-listpack-entries 64 appendonly no}} {

    test {disassembly-threshold prunes single-use templates on RDB load (simple scenario)} {
        r flushall

        # Field set {a b c d} is shared by three keys; {p q r s} by one.
        r hset shared1 a 1 b 2 c 3 d 4
        r hset shared2 a 5 b 6 c 7 d 8
        r hset shared3 a 9 b 8 c 7 d 6
        r hset lone    p 1 q 2 r 3 s 4
        assert_equal 0 [s hash_templates]

        # Convert on load (>= 4 fields) but keep only templates with >= 3 keys.
        r config set hash-rdb-load-min-template-entries 4
        r config set hash-rdb-load-template-disassembly-threshold 3
        r config rewrite
        r save
        restart_server 0 true false

        # The shared field set graduates (3 keys) and survives as a template; the
        # single-use one is disassembled back to a plain listpack.
        assert_equal template-listpack [r object encoding shared1]
        assert_equal template-listpack [r object encoding shared2]
        assert_equal template-listpack [r object encoding shared3]
        assert_equal listpack          [r object encoding lone]
        assert_equal 1 [s hash_templates]
        assert_equal 3 [s hash_template_keys]
        assert_equal 4 [r hget shared1 d]
        assert_equal 6 [r hget shared3 d]
        assert_equal 4 [r hget lone s]

        # Exactly the one single-use template (1 key) was disassembled.
        verify_log_message 0 "*disassembled 1 templates (1 keys) back to plain hashes*" 0
    }

    test {disassembly on mixed RDB across both encodings (complex scenario)} {
        r flushall
        wait_num_templates 0

        set small {1 2 3 4}
        set big [list 1 2 3 [string repeat x 80]]   ;# a >64B value forces hashtable

        # 10 field sets shared by 2 keys each (will survive): 5 listpack, 5 hashtable.
        for {set t 0} {$t < 10} {incr t} {
            set fields [list s${t}_a s${t}_b s${t}_c s${t}_d]
            if {$t < 5} { set vals $small } else { set vals $big }
            foreach k {0 1} {
                set pairs {}
                foreach f $fields v $vals { lappend pairs $f $v }
                r hset surv_${t}_$k {*}$pairs
            }
        }
        # 3 single-use field sets (will be pruned): 2 listpack, 1 hashtable.
        for {set t 0} {$t < 3} {incr t} {
            set fields [list p${t}_a p${t}_b p${t}_c p${t}_d]
            if {$t < 2} { set vals $small } else { set vals $big }
            set pairs {}
            foreach f $fields v $vals { lappend pairs $f $v }
            r hset lone_$t {*}$pairs
        }

        # Still plain before the load-time conversion; snapshot the dataset.
        assert_equal listpack [r object encoding surv_0_0]
        assert_equal hashtable [r object encoding surv_5_0]
        assert_equal 0 [s hash_templates]
        set digest [r debug digest]

        # Convert on load (>= 4 fields), keep only templates shared by >= 2 keys.
        r config set hash-rdb-load-min-template-entries 4
        r config set hash-rdb-load-template-disassembly-threshold 2
        r config rewrite
        r save
        restart_server 0 true false

        # Survivors:
        assert_equal template-listpack [r object encoding surv_0_0]
        assert_equal template-listpack [r object encoding surv_4_1]
        assert_equal template-array    [r object encoding surv_5_0]
        assert_equal template-array    [r object encoding surv_9_1]
        assert_equal 10 [s hash_templates]
        assert_equal 20 [s hash_template_keys]

        # Pruned: disassembled back to plain hashes
        assert_equal listpack [r object encoding lone_0]
        assert_equal listpack [r object encoding lone_1]
        assert_equal hashtable [r object encoding lone_2]
        verify_log_message 0 "*disassembled 3 templates (3 keys) back to plain hashes*" 0

        # Whole dataset intact: digest matches, plus a few spot checks.
        assert_equal $digest [r debug digest]
        assert_equal 4 [r hget surv_0_0 s0_d]
        assert_equal [string repeat x 80] [r hget surv_9_1 s9_d]
        assert_equal 4 [r hget lone_0 p0_d]
        assert_equal [string repeat x 80] [r hget lone_2 p2_d]
    }

    test {disassembly-threshold=0 keeps every converted template} {
        r flushall
        wait_num_templates 0
        
        # A single-use field set that would be pruned if the threshold were on.
        r hset solo a 1 b 2 c 3 d 4
        assert_equal 0 [s hash_templates]
        r config set hash-rdb-load-min-template-entries 4
        r config set hash-rdb-load-template-disassembly-threshold 0
        r config rewrite
        r save
        restart_server 0 true false

        # Threshold disabled: the single-use template is kept.
        assert_equal template-listpack [r object encoding solo]
        assert_equal 1 [s hash_templates]
        assert_equal 1 [s hash_template_keys]
        assert_equal 4 [r hget solo d]
    }

    test {disassembly tolerates a throttle-triggering RDB load (simple scenario)} {
        # 1100 keys, each with a unique field set. On load this trips the template
        # creation throttle (too many single-use templates). The load must still
        # finish with all data intact, and since no field set is shared, no
        # template survives.
        r flushall
        wait_num_templates 0
        for {set i 0} {$i < 1100} {incr i} {
            r hset k$i ${i}_a 1 ${i}_b 2 ${i}_c 3 ${i}_d 4
        }
        assert_equal 0 [s hash_templates]
        r config set hash-rdb-load-min-template-entries 4
        r config set hash-rdb-load-template-disassembly-threshold 2
        r config rewrite
        r save
        restart_server 0 true false
        
        # The load crossed the throttle and logged it.
        verify_log_message 0 "*Hash template creation throttled during RDB load*" 0

        # Every field set was single-use, so none survive as templates whether
        # they were throttled (never created) or disassembled at end of load.
        assert_equal 0 [s hash_templates]
        assert_equal 0 [s hash_template_keys]
        assert_equal listpack [r object encoding k0]
        assert_equal listpack [r object encoding k1099]
        assert_equal 4 [r hget k0 0_d]
        assert_equal 4 [r hget k1099 1099_d]
    }

    test {throttle latches but heavily-shared field sets still survive (complex scenario)} {
        r flushall
        wait_num_templates 0

        set small {1 2 3 4}
        set big   [list 1 2 3 [string repeat x 80]]

        # 6 field sets shared by 50 keys each: created early, keep attaching even
        # after the throttle latches, so they graduate and survive. 3 lp, 3 ht.
        # 50 keys makes survival load-order independent (all 50 landing past the
        # latch is astronomically unlikely), so the counts below are stable.
        for {set g 0} {$g < 6} {incr g} {
            set fields [list sh${g}_a sh${g}_b sh${g}_c sh${g}_d]
            if {$g < 3} { set vals $small } else { set vals $big }
            for {set j 0} {$j < 50} {incr j} {
                set pairs {}
                foreach f $fields v $vals { lappend pairs $f $v }
                r hset sh${g}_k$j {*}$pairs
            }
        }

        # 1100 distinct single-use field sets: enough to latch the conversion throttle.
        for {set i 0} {$i < 1100} {incr i} {
            r hset lone$i ${i}_a 1 ${i}_b 2 ${i}_c 3 ${i}_d 4
        }
        assert_equal 0 [s hash_templates]
        set digest [r debug digest]

        r config set hash-rdb-load-min-template-entries 4
        r config set hash-rdb-load-template-disassembly-threshold 2
        r config rewrite
        r save
        restart_server 0 true false

        # The throttle latched partway through the load.
        verify_log_message 0 "*Hash template creation throttled during RDB load*" 0

        # The 6 heavily-shared field sets survived as templates
        assert_equal 6 [s hash_templates]
        assert_equal 300 [s hash_template_keys]
        assert_equal template-listpack [r object encoding sh0_k0]
        assert_equal template-array    [r object encoding sh3_k0]
        assert_equal listpack [r object encoding lone0]
        assert_equal listpack [r object encoding lone1099]

        # Whole dataset intact.
        assert_equal $digest [r debug digest]
        assert_equal 4 [r hget sh0_k0 sh0_d]
        assert_equal [string repeat x 80] [r hget sh3_k49 sh3_d]
        assert_equal 4 [r hget lone0 0_d]
    }

    test {converting hash expired on load is dropped without leaking its template} {
        r flushall
        wait_num_templates 0

        # Keep the past-TTL key in the keyspace until SAVE writes it; the load
        # then discards it on master startup (expiretime < now). The expired key
        # is dropped before dbAddRDBLoad, so it is never recorded for disassembly.
        r debug set-active-expire 0

        # Shared field set {a b c d}: two keys, kept as one template after load.
        r hset live1 a 1 b 2 c 3 d 4
        r hset live2 a 5 b 6 c 7 d 8

        # Unique field set with a short TTL that is past by load time. It converts
        # during load, then is discarded before dbAddRDBLoad, so it is never
        # recorded; its single-use template must not survive.
        r hset gone w 1 x 2 y 3 z 4
        r pexpire gone 100
        assert_equal 0 [s hash_templates]
        r config set hash-rdb-load-min-template-entries 4
        r config set hash-rdb-load-template-disassembly-threshold 2
        r config rewrite
        r save
        restart_server 0 true false

        # Expired key gone; only the shared field set survives as a template.
        assert_equal 0 [r exists gone]
        assert_equal template-listpack [r object encoding live1]
        assert_equal template-listpack [r object encoding live2]
        assert_equal 1 [s hash_templates]
        assert_equal 2 [s hash_template_keys]
        assert_equal 4 [r hget live1 d]
        assert_equal 8 [r hget live2 d]
    }
}

# When end-of-load disassembly converts a template hash back to a plain
# listpack/hashtable in place, the key's memory size changes. Redis keeps a
# per-slot allocation-size histogram that must stay in sync with that change,
# or its consistency check fails on the next write to the slot. That histogram
# only exists when key-memory-histograms is enabled at startup.
start_server {tags {"hash" "needs:debug" "cluster:skip" "external:skip"}
              overrides {hash-min-template-entries 0 hash-max-listpack-entries 64
                         appendonly no key-memory-histograms yes}} {

    test {disassembly keeps the per-slot alloc-size histogram consistent} {
        r flushall
        
        # Sub-threshold field sets (one key each) so both get disassembled:
        #  - lp4: small => TMPL_LP -> disassembled back to LISTPACK
        #  - big: many large values => TMPL_ARRAY -> disassembled back to HT
        r hset lp4 a 1 b 2 c 3 d 4
        for {set i 0} {$i < 200} {incr i} { r hset big f$i [string repeat x 80] }
        r config set hash-rdb-load-min-template-entries 4
        r config set hash-rdb-load-template-disassembly-threshold 2
        r config rewrite
        r save
        restart_server 0 true false

        # Both single-use templates disassembled back to plain encodings.
        assert_equal listpack  [r object encoding lp4]
        assert_equal hashtable [r object encoding big]
        assert_equal 0 [s hash_templates]

        # Enable the per-slot consistency check, then write to each key: a
        # desynced histogram would panic here instead of replying.
        r debug allocsize-slots-assert 1
        r set trigger 1
        assert_equal 4 [r hget lp4 d]
        r hset lp4 e 5
        r hset big f0 [string repeat y 80]
        assert_equal PONG [r ping]
        r debug allocsize-slots-assert 0
    }
}

# An RDB that already contains templates must skip load-time conversion
# entirely: plain hashes are left plain (no rdb-load-min conversion) and its
# templates are restored as-is (no disassembly), whatever the configs say.
start_server {tags {"hash" "needs:debug" "cluster:skip" "external:skip"}
              overrides {hash-min-template-entries 4 hash-max-listpack-entries 64 appendonly no}} {

    test {RDB with templates skips both load-time conversion and disassembly} {
        r flushall
        wait_num_templates 0
        r config set hash-min-template-entries 0   ;# no runtime auto-convert

        # One explicit template -> the RDB carries a template record.
        r himport prepare fs a b c d
        r himport set tmpl fs 1 2 3 4
        assert_equal template-listpack [r object encoding tmpl]

        r hset plain e 1 f 2 g 3 h 4
        assert_equal listpack [r object encoding plain]
        assert_equal 1 [s hash_templates]

        # Enable both load-time conversion and a pruning threshold...
        r config set hash-rdb-load-min-template-entries 4
        r config set hash-rdb-load-template-disassembly-threshold 100
        r config rewrite
        r save
        restart_server 0 true false

        # Existing template header turns both config off: the template survives
        # (not disassembled) and the plain hash is NOT converted.
        assert_equal template-listpack [r object encoding tmpl]
        assert_equal listpack [r object encoding plain]
        assert_equal 1 [s hash_templates]
        assert_equal 1 [s hash_template_keys]
        assert_equal 4 [r hget tmpl d]
        assert_equal 4 [r hget plain h]
    }
}

# RESTORE deep validation (sanitize-dump-payload yes).
# A template payload must carry strictly sorted field names
start_server {tags {"hash" "needs:debug" "cluster:skip" "external:skip"}
              overrides {hash-min-template-entries 0
                         sanitize-dump-payload yes
                         loglevel debug}} {

    r debug set-skip-checksum-validation 1

    # Build a fresh TMPL_LP / TMPL_ARRAY dump for fields field1,field2 -> 1,2.
    # The field names are long and distinctive so the byte substitutions below
    # only ever hit the field-name bytes, never the value or footer bytes.
    proc tmpl_dump {enc} {
        r del rk
        catch {r himport discard fieldset}
        r himport prepare fieldset field1 field2
        r himport set rk fieldset 1 2
        assert_equal [r object encoding rk] $enc
        set dump [r dump rk]
        r del rk
        r himport discard fieldset
        return $dump
    }

    foreach {enc maxlp} {template-listpack 128 template-array 0} {
        r config set hash-max-listpack-entries $maxlp

        test "RESTORE deep validation: $enc accepts sorted fields" {
            set dump [tmpl_dump $enc]
            r restore rk 0 $dump
            assert_equal [r object encoding rk] $enc
            assert_equal [r hgetall rk] {field1 1 field2 2}
            r del rk
        }

        test "RESTORE deep validation: $enc rejects out-of-order fields" {
            set dump [tmpl_dump $enc]
            # Swap the names so the stored fields become descending (field2, field1).
            set bad [string map {field1 field2 field2 field1} $dump]
            set loglines [count_log_lines 0]
            assert_error "*Bad data format*" {r restore rk 0 $bad}
            wait_for_log_messages 0 {"*fields not strictly sorted*"} $loglines 50 100
        }

        test "RESTORE deep validation: $enc rejects duplicate fields" {
            set dump [tmpl_dump $enc]
            set bad [string map {field1 field2} $dump]
            set loglines [count_log_lines 0]
            assert_error "*Bad data format*" {r restore rk 0 $bad}
            wait_for_log_messages 0 {"*fields not strictly sorted*"} $loglines 50 100
        }
    }
}

start_server {tags {"hash" "cluster:skip" "external:skip"} overrides {loglevel debug}} {
    test "RESTORE rejects malformed template payload: value count != field count" {
        # Values listpack has 2 entries (id + 1 value); needs field_count+1 = 3.
        set bad "\x1d\x00\x17\x17\x00\x00\x00\x02\x00\x86\x66\x69\x65\x6c\x64\x31\x07\x86\x66\x69\x65\x6c\x64\x32\x07\xff\x0b\x0b\x00\x00\x00\x02\x00\x00\x01\x01\x01\xff\x0f\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        set ll [count_log_lines 0]
        assert_error "*Bad data format*" {r restore rk 0 $bad}
        wait_for_log_messages 0 {"*template-listpack entry count*does not match*"} $ll 50 100
    }

    test "RESTORE rejects malformed template payload: empty values listpack" {
        # Values listpack is valid but empty.
        set bad "\x1d\x00\x17\x17\x00\x00\x00\x02\x00\x86\x66\x69\x65\x6c\x64\x31\x07\x86\x66\x69\x65\x6c\x64\x32\x07\xff\x07\x07\x00\x00\x00\x00\x00\xff\x0f\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        set ll [count_log_lines 0]
        assert_error "*Bad data format*" {r restore rk 0 $bad}
        wait_for_log_messages 0 {"*template-listpack is empty*"} $ll 50 100
    }

    test "RESTORE rejects malformed template payload: non-integer template id" {
        set bad "\x1d\x00\x17\x17\x00\x00\x00\x02\x00\x86\x66\x69\x65\x6c\x64\x31\x07\x86\x66\x69\x65\x6c\x64\x32\x07\xff\x0e\x0e\x00\x00\x00\x03\x00\x81\x78\x02\x01\x01\x02\x01\xff\x0f\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        set ll [count_log_lines 0]
        assert_error "*Bad data format*" {r restore rk 0 $bad}
        wait_for_log_messages 0 {"*first entry is not an integer*"} $ll 50 100
    }

    test "RESTORE rejects malformed template payload: TMPL_LP zero fields" {
        set bad "\x1d\x00\x07\x07\x00\x00\x00\x00\x00\xff\x0d\x0d\x00\x00\x00\x03\x00\x00\x01\x01\x01\x02\x01\xff\x0f\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        set ll [count_log_lines 0]
        assert_error "*Bad data format*" {r restore rk 0 $bad}
        wait_for_log_messages 0 {"*template with zero fields*"} $ll 50 100
    }

    test "RESTORE rejects malformed template payload: TMPL_ARRAY zero fields" {
        # Raw field count byte (offset 2) set to 0.
        set bad "\x1f\x01\x00\x06\x66\x69\x65\x6c\x64\x31\x06\x66\x69\x65\x6c\x64\x32\xc0\x01\xc0\x02\x0f\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        set ll [count_log_lines 0]
        assert_error "*Bad data format*" {r restore rk 0 $bad}
        wait_for_log_messages 0 {"*template with zero fields*"} $ll 50 100
    }

    # The REF encodings (TMPL_LP_REF / TMPL_ARRAY_REF) only exist inside a RDB
    # file. RESTORE must reject them.
    test "RESTORE rejects REF-encoded template hash types" {
        set reflp "\x1e\x0d\x0d\x00\x00\x00\x03\x00\x05\x01\x01\x01\x02\x01\xff\x0f\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        set refarr "\x20\x05\xc0\x01\xc0\x02\x0f\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        set ll [count_log_lines 0]
        assert_error "*Bad data format*" {r restore refk1 0 $reflp}
        assert_error "*Bad data format*" {r restore refk2 0 $refarr}
        assert_equal 0 [r exists refk1 refk2]
        wait_for_log_messages 0 {"*not valid in a DUMP payload*"} $ll 50 100
    }
}

# RDB file corruption for the REF encodings (TMPL_LP_REF/TMPL_ARRAY_REF) and the
# RDB_OPCODE_HASH_TEMPLATE section. These forms exist only in a full RDB file
# (not DUMP/RESTORE), so we save a good template RDB, corrupt specific bytes and
# assert the server aborts loading it with the expected corruption message.
start_server {tags {"hash" "needs:debug" "cluster:skip" "external:skip" "debug_defrag:skip"}
              overrides {save ""}} {
    set server_path [lindex [r config get dir] 1]
    set rdbfile [file join $server_path [lindex [r config get dbfilename] 1]]

    # Save two keys sharing one template and return rdb bytes.
    proc build_rdb {enc} {
        upvar rdbfile rdbfile
        r config set hash-max-listpack-entries [expr {$enc eq "template-listpack" ? 128 : 0}]
        r flushall
        r himport prepare fs field1 field2 field3 field4
        r himport set k1 fs 1 2 3 4
        r himport set k2 fs 5 6 7 8
        r himport discard fs
        assert_encoding $enc k1
        r save
        set f [open $rdbfile r]; fconfigure $f -translation binary
        set bytes [read $f]; close $f
        return $bytes
    }

    # Overwrite the on-disk RDB with $bytes, start a server that loads it, assert
    # it aborts with a log line matching $pattern.
    proc assert_rdb_load_fails {bytes pattern} {
        upvar rdbfile rdbfile server_path server_path
        set f [open $rdbfile w]; fconfigure $f -translation binary; puts -nonewline $f $bytes; close $f
        set srv [start_server [list overrides [list dir $server_path save {}] keep_persistence true]]
        wait_for_condition 50 100 {
            [string match $pattern [exec tail -20 < [dict get $srv stdout]]]
        } else {
            fail "server did not reject RDB: [exec tail -5 < [dict get $srv stdout]]"
        }
        kill_server $srv
    }

    test {RDB load rejects TMPL_LP REF hash with invalid template id} {
        set good [build_rdb template-listpack]
        # Patch k2's template id 0 -> 5: the id is the values-lp first entry, 7 bytes
        # (1B strlen + 4B lp-bytes + 2B lp-len) past the "\x1e\x02k2" record prefix.
        set rec "\x1e\x02k2"
        set pos [expr {[string first $rec $good] + [string length $rec] + 7}]
        assert_rdb_load_fails [string replace $good $pos $pos "\x05"] "*Invalid hash template ID 5*"
    }

    test {RDB load rejects TMPL_ARRAY REF hash with invalid template id} {
        set good [build_rdb template-array]
        # Patch k2's template id 0 -> 5: the id is the byte right after "\x20\x02k2".
        set rec "\x20\x02k2"
        set pos [expr {[string first $rec $good] + [string length $rec]}]
        assert_rdb_load_fails [string replace $good $pos $pos "\x05"] "*Invalid hash template ID 5*"
    }

    test {RDB load rejects template section with zero fields} {
        set good [build_rdb template-listpack]
        # Zero the template's field-count byte
        set pos [expr {[string first "field1" $good] - 2}]
        assert_rdb_load_fails [string replace $good $pos $pos "\x00"] "*has zero fields*"
    }

    test {RDB load rejects template section with unsorted fields} {
        set good [build_rdb template-listpack]
        # Rename field1 so the section's field names are no longer ascending.
        assert_rdb_load_fails [string map {field1 fieldZ} $good] "*not strictly sorted*"
    }
}


# When a template hash is freed in a background thread (async flush / lazyfree),
# its key_refcount drop is not applied there but recorded as a pending drop and
# collected lazily by the main thread in cron. Until that happens the template
# still counts the key, so an RDB save can write it even though none of its keys
# reach the RDB. On reload it then has no referencing key and must be deleted,
# not left in the registry. Reproduced deterministically here by expiring the 
# template's only key: reload drops the expired key and deletes the template.
start_server {tags {"hash" "needs:debug" "cluster:skip" "external:skip"}} {
    test {RDB reload prunes a template left unreferenced by an expired key} {
        r flushall
        r debug set-active-expire 0
        r himport prepare fs 1000 2000 3000 4000
        r himport set k1 fs a b c d
        r himport discard fs
        assert_encoding template-listpack k1
        assert_equal 1 [s hash_templates]
        assert_equal {a b c d} [r hmget k1 1000 2000 3000 4000]
        # Expire k1 in the past: the save still writes the template (key_refcount 1)
        # and k1, but reload drops k1 as expired, leaving the template unreferenced.
        r pexpireat k1 1
        r debug reload
        assert_equal 0 [r dbsize]
        assert_equal 0 [s hash_templates]
        r debug set-active-expire 1
    }
}


# Chained replication A->B->C: RESTORE (the propagated form of HIMPORT SET) must
# flow down the chain and reconstruct the template hash on every node.
start_server {tags {"hash" "repl" "needs:repl" "cluster:skip" "external:skip"}
              overrides {hash-min-template-entries 0}} {
    start_server {overrides {hash-min-template-entries 0}} {
        start_server {overrides {hash-min-template-entries 0}} {
            test {Chained replication propagates template hashes A->B->C} {
                set a [srv -2 client]
                set b [srv -1 client]
                set c [srv 0 client]

                $b replicaof [srv -2 host] [srv -2 port]
                $c replicaof [srv -1 host] [srv -1 port]
                wait_for_sync $b
                wait_for_sync $c

                $a himport prepare fieldset name email
                $a himport set chain:1 fieldset alice alice@x.com

                wait_for_condition 50 100 { [$c exists chain:1] == 1 } else {
                    fail "template hash not propagated to C"
                }
                assert_equal [$c hgetall chain:1] {name alice email alice@x.com}
                assert_equal [$c object encoding chain:1] template-listpack
                assert_equal [$b hget chain:1 email] alice@x.com
            }
        }
    }
}

# Stress test of large template-based keys, end-to-end, with a replica attached. 
# For a range of field counts and both template encodings, exercise
# HIMPORT PREPARE/SET (which replicate as RESTORE), HRANDFIELD, HSET of a new 
# field and HDEL. After every mutation the entire key (all fields and values) 
# and its template encoding are verified on both the master and the replica. 
start_server {tags {"hash" "repl" "needs:repl" "needs:debug" "cluster:skip" "external:skip"}
              overrides {hash-min-template-entries 0}} {
    start_server {overrides {hash-min-template-entries 0}} {
        set replica [srv -1 client]
        $replica replicaof [srv 0 host] [srv 0 port]
        wait_for_sync $replica

        foreach {enc maxlp} {template-listpack 2048 template-array 0} {
            foreach field_count {127 128 129 256 512 1024 2048} {
                test "large template-based key: $field_count fields ($enc)" {
                    r config set hash-max-listpack-entries $maxlp
                    $replica config set hash-max-listpack-entries $maxlp
                    r flushall

                    # Build $field_count fields f0000.. -> val_f0000..
                    set fields {}; set vals {}; set flat {}
                    for {set i 0} {$i < $field_count} {incr i} {
                        set f f[format %04d $i]
                        lappend fields $f
                        lappend vals val_$f
                        lappend flat $f val_$f
                    }
                    set expected [lsort $flat]

                    r himport prepare fieldset {*}$fields
                    r himport set k fieldset {*}$vals

                    # HRANDFIELD returns all fields with their correct values.
                    set rand [r hrandfield k $field_count WITHVALUES]
                    assert_equal [expr {$field_count * 2}] [llength $rand]
                    foreach {f v} $rand { assert_equal val_$f $v }
                    assert_equal $field_count [llength [lsort -unique [r hrandfield k $field_count]]]

                    # Master and replica: template encoded, every field/value correct.
                    assert_equal $enc [r object encoding k]
                    assert_equal $expected [lsort [r hgetall k]]
                    wait_for_condition 50 100 {
                        [lsort [$replica hgetall k]] eq $expected
                    } else { fail "Replica out of sync after HIMPORT SET" }
                    assert_match {template-*} [$replica object encoding k]

                    # HSET a new field: re-verify the whole key everywhere.
                    set grown [lsort [concat $flat new_field newval]]
                    r hset k new_field newval
                    assert_match {template-*} [r object encoding k]
                    assert_equal $grown [lsort [r hgetall k]]
                    wait_for_condition 50 100 {
                        [lsort [$replica hgetall k]] eq $grown
                    } else { fail "Replica out of sync after HSET" }
                    assert_match {template-*} [$replica object encoding k]

                    # HDEL the new field: the key is back to its original contents.
                    assert_equal 1 [r hdel k new_field]
                    assert_match {template-*} [r object encoding k]
                    assert_equal $expected [lsort [r hgetall k]]
                    wait_for_condition 50 100 {
                        [lsort [$replica hgetall k]] eq $expected
                    } else { fail "Replica out of sync after HDEL" }
                    assert_match {template-*} [$replica object encoding k]

                    # Survives an RDB reload with all fields/values intact.
                    r debug reload
                    assert_match {template-*} [r object encoding k]
                    assert_equal $expected [lsort [r hgetall k]]
                }
            }
        }
    }
}


# Tests with large amount of templates: Distinct templates must round-trip
# through full sync, AOF restart and RDB restart without loss or corruption.
# Each key has its own unique field set (a per-key suffix in the field names),
# so the server tracks ~$bulk_n distinct templates at once far more than the
# single-template tests above. Correctness is checked end-to-end with DEBUG
# DIGEST plus the hash_templates / hash_template_keys INFO counters.
set ::bulk_n 2000

# Populate 'n' keys on 'client', each with its own 5-field template. The per-key
# suffix in the field names makes every field set unique -> a distinct template.
proc populate_distinct_templates {client n} {
    for {set i 0} {$i < $n} {incr i} {
        $client himport prepare fieldset$i \
            f${i}_user_id f${i}_email f${i}_score f${i}_status f${i}_country
        $client himport set bulk:$i fieldset$i \
            [expr {100000 + $i}] "user$i@example.com" [expr {$i % 1000}] \
            [expr {$i % 2 == 0 ? "active" : "inactive"}] "Country[expr {$i % 195}]"
    }
}

start_server {tags {"hash" "repl" "needs:repl" "needs:debug" "cluster:skip" "external:skip"}
              overrides {hash-min-template-entries 0 save {}}} {
    start_server {overrides {hash-min-template-entries 0}} {
        test "Full sync replicates $::bulk_n distinct templates" {
            set master [srv -1 client]
            set master_host [srv -1 host]
            set master_port [srv -1 port]
            set replica [srv 0 client]

            $master flushall
            wait_num_template_keys 0 -1
            populate_distinct_templates $master $::bulk_n

            set T [s -1 hash_templates]
            set K [s -1 hash_template_keys]
            assert_equal $T $::bulk_n
            assert_equal $K $::bulk_n
            set digest [$master debug digest]
            set sync_full_before [s -1 sync_full]

            # Replica attaches now, so the whole dataset arrives via a full sync.
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            assert {[s -1 sync_full] > $sync_full_before}

            # INFO must show the registry was actually rebuilt on the replica
            wait_num_templates $T 0
            wait_num_template_keys $K 0
            # dataset must be byte-identical to the master.
            assert_equal $digest [$replica debug digest]
            assert_equal [lsort [$replica hgetall bulk:0]] \
                         [lsort [$master hgetall bulk:0]]
        }
    }
}

start_server {tags {"hash" "needs:debug" "cluster:skip" "external:skip"}
              overrides {hash-min-template-entries 0 appendonly yes
                         auto-aof-rewrite-percentage 0 save {}}} {
    test "AOF restart preserves $::bulk_n distinct templates" {
        r flushall
        wait_num_template_keys 0
        waitForBgrewriteaof r
        populate_distinct_templates r $::bulk_n

        set T [s hash_templates]
        set K [s hash_template_keys]
        set digest [r debug digest]

        # Fold everything into a fresh base AOF, then restart off disk.
        r bgrewriteaof
        waitForBgrewriteaof r
        restart_server 0 true false

        wait_num_templates $T
        wait_num_template_keys $K
        assert_equal $digest [r debug digest]
        assert_equal [r hget bulk:0 f0_user_id] 100000
    }
}

start_server {tags {"hash" "needs:debug" "cluster:skip" "external:skip"}
              overrides {hash-min-template-entries 0 appendonly no save {900 1}}} {
    test "RDB restart preserves $::bulk_n distinct templates" {
        r flushall
        wait_num_template_keys 0
        populate_distinct_templates r $::bulk_n

        set T [s hash_templates]
        set K [s hash_template_keys]
        set digest [r debug digest]

        # DUMP/RESTORE of one key exercises the full template serialization
        # format and its field-order validation on load.
        set blob [r dump bulk:1]
        r restore bulk:copy 0 $blob
        assert_equal [lsort [r hgetall bulk:1]] [lsort [r hgetall bulk:copy]]
        r del bulk:copy

        r save
        restart_server 0 true false

        wait_num_templates $T
        wait_num_template_keys $K
        assert_equal $digest [r debug digest]
        assert_equal [r hget bulk:0 f0_email] "user0@example.com"
    }
}

start_server {tags {"hash" "needs:debug" "cluster:skip"} overrides {hash-min-template-entries 0}} {
    # DUMP/RESTORE round-trip covering every combination of value encoding and
    # field-name length. The value encoding is template-array when a value is too
    # big for a listpack (otherwise template-listpack); the field names are stored
    # individually when a name is too big for a listpack (otherwise as one lp blob).
    # A >64-byte value or field name triggers each case, so the four short/long
    # combinations exercise all of them.
    set long_field [string repeat n 70]   ;# a >64-byte field name -> names stored individually
    set long_val   [string repeat x 70]   ;# a >64-byte value      -> template-array encoding
    #        variant           fields                     values                    encoding           expected hgetall
    foreach {variant           fields                     vals                      enc                expect} [list \
        lp_short_fields  [list f0 f1 f2]            [list v1 v2 v3]           template-listpack  [list f0 v1 f1 v2 f2 v3] \
        arr_long_fields  [list f0 f1 $long_field]   [list v1 $long_val v3]    template-array     [list f0 v1 f1 $long_val $long_field v3] \
        lp_long_fields   [list f0 f1 $long_field]   [list v1 v2 v3]           template-listpack  [list f0 v1 f1 v2 $long_field v3] \
        arr_short_fields [list f0 f1 f2]            [list v1 $long_val v3]    template-array     [list f0 v1 f1 $long_val f2 v3] \
    ] {
        test "DUMP/RESTORE preserves template encoding: $variant" {
            r himport prepare fs_$variant {*}$fields
            r himport set k_$variant fs_$variant {*}$vals
            assert_encoding $enc k_$variant

            set d [r dump k_$variant]
            r del k_$variant
            r restore k_$variant 0 $d                 ;# round-trip into the same key
            assert_encoding $enc k_$variant
            assert_equal $expect [r hgetall k_$variant]

            r restore k_${variant}_copy 0 $d          ;# and into a different key
            assert_encoding $enc k_${variant}_copy
            assert_equal $expect [r hgetall k_${variant}_copy]
        }
    }
}
