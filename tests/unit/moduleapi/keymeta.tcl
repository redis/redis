# ============================================================================
# Key Metadata (keymeta) Test Suite
# ============================================================================
#
# Tests the Redis module key metadata framework: up to 7 independent metadata
# classes (IDs 1-7) can be attached to keys. Class ID 0 is reserved for key
# expiration.
#
# The following features are sensitive to Key Metadata and are tested here:
#
# - KEY EXPIRATION (class ID 0)
#   - Stored at ((uint64_t *)kv) - 1 (first metadata slot)
#   - Managed via db->expires dictionary
#   - Must be preserved/updated when kvobj is reallocated
# - HASH FIELD EXPIRATION (HFE)
#   - NOT in kvobj metadata slots (Maybe in the future...)
#   - Managed via db->hexpires ebuckets (holds direct kvobj pointer)
#   - Must be removed before kvobj reallocation (hashTypeRemoveFromExpires)
#      and restored after (hashTypeAddToExpires)
# - MODULE METADATA (class IDs 1-7)
#   - Defines metadata lifecycle via callbacks
# - EMBEDDED STRINGS vs. REGULAR OBJECTS
#   - Short strings and numbers are embedded into kvobj
#   - The rest are kept as distinct objects
# - LAZYFREE
# ============================================================================

set testmodule [file normalize tests/modules/test_keymeta.so]

# Helper procedure to convert class ID to 4-char-id name
proc cname {cid} {
    return "KMT$cid"
}

# Helper procedure to check if a class should keep metadata for a given operation
proc shouldKeep {cid operation classesSpec} {
    upvar $classesSpec specs
    set spec $specs($cid)
    switch $operation {
        "copy"   { return [string match "*KEEPONCOPY*" $spec] }
        "rename" { return [string match "*KEEPONRENAME*" $spec] }
        "move"   { return [string match "*KEEPONMOVE*" $spec] }
        default  { return 0 }
    }
}

# Helper procedure to setup a key with metadata
proc setupKeyMeta {keyname numClasses expiryBefore expiryAfter} {
    # Set expiry if requested
    if {$expiryBefore} {
        r expire $keyname 10000
        assert_range [r ttl $keyname] 9990 10000
    }

    # Set metadata for all classes
    for {set i 1} {$i <= $numClasses} {incr i} {
        # Set twice to verify overwrite behavior
        r keymeta.set [cname $i] $keyname "blabla$i"
        assert_equal [r keymeta.get [cname $i] $keyname] "blabla$i"
        r keymeta.set [cname $i] $keyname "meta$i"
    }

    # Verify metadata was set correctly
    for {set i 1} {$i <= $numClasses} {incr i} {
        assert_equal [r keymeta.get [cname $i] $keyname] "meta$i"
    }

    if {$expiryAfter} {
        r expire $keyname 10000
        assert_range [r ttl $keyname] 9990 10000
    }

    if {$expiryBefore} {
        assert_range [r ttl $keyname] 9990 10000
    }
}

# Helper procedure to verify metadata after an operation
proc verifyKeyMeta {keyname operation numClasses hasExpiry classesSpec} {
    upvar $classesSpec specs

    # Verify expiry
    if {$hasExpiry} {
        assert_range [r ttl $keyname] 9990 10000
    }

    # Verify metadata based on class spec
    for {set i 1} {$i <= $numClasses} {incr i} {
        set expected [expr {[shouldKeep $i $operation specs] ? "meta$i" : ""}]
        assert_equal [r keymeta.get [cname $i] $keyname] $expected
    }
}

proc flushallAndVerifyCleanup {} {
    r flushall
    # Verify all metadata is cleaned up properly
    assert_equal [r keymeta.active] 0
}

start_server {tags {"modules" "external:skip" "cluster:skip"} overrides {enable-debug-command yes}} {
    r module load $testmodule

    array set classesSpec {}
    set classesSpec(1) "KEEPONCOPY:KEEPONRENAME:KEEPONMOVE:ALLOWIGNORE:RDBLOAD:RDBSAVE"
    set classesSpec(2) "KEEPONCOPY:KEEPONRENAME:UNLINKFREE:ALLOWIGNORE:RDBLOAD:RDBSAVE"
    set classesSpec(3) "KEEPONCOPY:ALLOWIGNORE:RDBLOAD:RDBSAVE"
    set classesSpec(4) "ALLOWIGNORE:RDBLOAD:RDBSAVE"
    set classesSpec(5) "KEEPONRENAME:KEEPONMOVE:ALLOWIGNORE:RDBLOAD:RDBSAVE"
    set classesSpec(6) "KEEPONRENAME:ALLOWIGNORE:RDBLOAD:RDBSAVE"
    set classesSpec(7) "KEEPONMOVE:UNLINKFREE:ALLOWIGNORE:RDBLOAD:RDBSAVE"

    array set classes {}
    for {set cid 1} {$cid <= 7} {incr cid} {
        set spec $classesSpec($cid)
        set classes($cid) [r keymeta.register [cname $cid] 1 $spec]
        puts "Registered class $cid with spec $spec"
        assert_equal $classes($cid) $cid
    }

    # Validates metadata behavior across COPY/RENAME/MOVE operations
    # with varying numbers of metadata classes (1-7), key expiration states,
    # key types (string/hash), hash field expiration, and metadata class flags
    # (KEEPONCOPY, KEEPONRENAME, KEEPONMOVE).
    for {set numClasses 1} {$numClasses < 8} {incr numClasses} {
        foreach expiryBefore {0 1} {
            foreach expiryAfter {0 1} {
                set hasExpiry [expr {$expiryBefore || $expiryAfter}]
                set expiryStr "expiryBefore=$expiryBefore, expiryAfter=$expiryAfter)"
                # Test COPY operation
                test "KEYMETA - copy key-string with $numClasses classes, $expiryStr" {
                    foreach value { 3 "value1" [string repeat "ABCD" 1000]} {
                        r select 0
                        r del k1 k2
                        r set k1 $value
                        setupKeyMeta k1 $numClasses $expiryBefore $expiryAfter
                        # Copy:
                        r copy k1 k2
                        # Verify:
                        assert_equal [r get k1] $value
                        assert_equal [r get k2] $value
                        # Verify expiry and metadata
                        verifyKeyMeta k2 "copy" $numClasses $hasExpiry classesSpec
                        flushallAndVerifyCleanup
                    }
                }

                test "KEYMETA - copy key-hash with $numClasses classes, $expiryStr" {
                    r select 0
                    r del h1 h2
                    r HSET h1 field1 "value1" field2 "value2"
                    r hexpire h1 10000 FIELDS 1 field1
                    setupKeyMeta h1 $numClasses $expiryBefore $expiryAfter
                    # Copy:
                    r copy h1 h2
                    # Verify:
                    verifyKeyMeta h2 "copy" $numClasses $hasExpiry classesSpec
                    assert_range [r httl h1 FIELDS 1 field1] 9999 10000
                    assert_range [r httl h2 FIELDS 1 field1] 9999 10000
                    flushallAndVerifyCleanup
                }

                # Test RENAME operation
                test "KEYMETA - rename key-string with $numClasses classes, $expiryStr" {
                    foreach value { 3 "value1" [string repeat "ABCD" 1000]} {
                        r select 0
                        r del k1 k2
                        r set k1 $value
                        setupKeyMeta k1 $numClasses $expiryBefore $expiryAfter
                        # Rename:
                        r rename k1 k2
                        # Verify:
                        assert_equal [r exists k1] 0
                        assert_equal [r get k2] $value
                        # Verify expiry and metadata
                        verifyKeyMeta k2 "rename" $numClasses $hasExpiry classesSpec
                        flushallAndVerifyCleanup
                    }
                }

                test "KEYMETA - rename key-hash with $numClasses classes, $expiryStr" {
                    r select 0
                    r del h1 h2
                    r HSET h1 field1 "value1" field2 "value2"
                    r hexpire h1 10000 FIELDS 1 field1
                    setupKeyMeta h1 $numClasses $expiryBefore $expiryAfter
                    # Rename:
                    r rename h1 h2
                    # Verify:
                    assert_equal [r exists h1] 0
                    assert_range [r httl h2 FIELDS 1 field1] 9999 10000
                    verifyKeyMeta h2 "rename" $numClasses $hasExpiry classesSpec
                    flushallAndVerifyCleanup
                }



                # Test MOVE operation
                test "KEYMETA - move key-string with $numClasses classes, $expiryStr" {
                    foreach value { 3 "value1" [string repeat "ABCD" 1000]} {
                        r select 9
                        r del k1
                        r select 0
                        r del k1
                        r set k1 $value
                        setupKeyMeta k1 $numClasses $expiryBefore $expiryAfter
                        # Perform move
                        assert_equal [r move k1 9] 1
                        # Verify key moved
                        assert_equal [r exists k1] 0
                        r select 9
                        assert_equal [r get k1] $value
                        # Verify expiry and metadata
                        verifyKeyMeta k1 "move" $numClasses $hasExpiry classesSpec
                        r select 0
                        flushallAndVerifyCleanup
                    }
                }

                test "KEYMETA - move key-hash with $numClasses classes, $expiryStr" {
                    r select 9
                    r del h1
                    r select 0
                    r del h1
                    r HSET h1 field1 "value1" field2 "value2"
                    r hexpire h1 10000 FIELDS 1 field1
                    setupKeyMeta h1 $numClasses $expiryBefore $expiryAfter
                    assert_range [r httl h1 FIELDS 1 field1] 9999 10000
                    assert_equal [r move h1 9] 1
                    assert_equal [r exists h1] 0
                    r select 9
                    assert_range [r httl h1 FIELDS 1 field1] 9999 10000
                    verifyKeyMeta h1 "move" $numClasses $hasExpiry classesSpec
                    r select 0
                    flushallAndVerifyCleanup
                }
            }
        }
    }

    test "KEYMETA - Verify active metadata count on copy" {
        for {set cid 1} {$cid < 7} {incr cid} {
            set numAlloc 0
            flushallAndVerifyCleanup
            set dupOnCopy [shouldKeep $cid "copy" classesSpec]
            r set k1 "v1"
            r keymeta.set [cname $cid] k1 "meta1"
            assert_equal [r keymeta.active] [incr numAlloc]
            r keymeta.set [cname $cid] k1 "meta1b"
            assert_equal [r keymeta.active] $numAlloc
            r copy k1 k1copy
            assert_equal [r keymeta.active] [incr numAlloc $dupOnCopy]
            r del k1
            assert_equal [r keymeta.active] [incr numAlloc -1]
            r del k1copy
            assert_equal [r keymeta.active] 0
        }
    }

    test "KEYMETA - Verify active metadata count on rename" {
        for {set cid 1} {$cid <= 7} {incr cid} {
            set numAlloc 0
            flushallAndVerifyCleanup
            set keepOnRename [shouldKeep $cid "rename" classesSpec]
            set discOnRename [expr {!$keepOnRename}]
            r set k1 "v1"
            r keymeta.set [cname $cid] k1 "meta1"
            assert_equal [r keymeta.active] [incr numAlloc]
            r rename k1 k1_renamed
            assert_equal [r keymeta.active] [incr numAlloc -$discOnRename]
            r del k1_renamed
            assert_equal [r keymeta.active] 0
        }
    }

    test "KEYMETA - Verify active metadata count on move" {
        for {set cid 1} {$cid <= 7} {incr cid} {
            set numAlloc 0
            r select 0
            flushallAndVerifyCleanup

            set keepOnMove [shouldKeep $cid "move" classesSpec]
            set discOnMove [expr {!$keepOnMove}]

            # Create keys with metadata in DB 0
            r set k1 "v1"
            r keymeta.set [cname $cid] k1 "meta1"
            assert_equal [r keymeta.active] [incr numAlloc]
            # Move: metadata discarded if !keepOnMove
            r move k1 9
            set active [r keymeta.active]
            assert_equal [r keymeta.active] [incr numAlloc -$discOnMove]
            # Cleanup
            r select 9
            r del k1
            r select 0
            assert_equal [r keymeta.active] 0
        }
    }

    test "KEYMETA - Verify metadta cleanup on lazyfree" {
        r config set lazyfree-lazy-user-del yes
        # Class 2 has UNLINKFREE flag, so it should call unlink callback when lazyfree is enabled
        # Class 1 does not have UNLINKFREE flag, so it should only call free callback
        foreach {cid} { 1 2 } {
            r config resetstat
            # Create a large unsorted set collection to ensure it exceeds LAZYFREE_THRESHOLD
            for {set i 0} {$i < 1024} {incr i} { r sadd myset $i }
            r keymeta.set [cname $cid] myset "meta"
            assert_equal [r keymeta.active] 1
            r del myset

            # Wait for lazyfree to complete and verify lazyfreed_objects incremented
            wait_for_condition 50 100 {
                [s lazyfree_pending_objects] == 0
            } else {
                fail "lazyfree isn't done"
            }
            assert_equal [r keymeta.active] 0
            assert_equal [s lazyfreed_objects] 1
        }
        r config set lazyfree-lazy-user-del no
    } {OK} {needs:config-resetstat}

    test "KEYMETA - Verify metadata cleanup on expire" {
        # Class 2 has UNLINKFREE flag, so it should call unlink callback when lazyfree is enabled
        # Class 1 does not have UNLINKFREE flag, so it should only call free callback
        foreach {cid} { 1 2 } {
            r set mykey "mykey$cid"
            r keymeta.set [cname $cid] mykey "meta"
            assert_equal [r keymeta.active] 1
            r pexpire mykey 1
            wait_for_condition 50 100 {
                [r exists mykey] == 0
            } else {
                fail "key not expired"
            }
            assert_equal [r keymeta.active] 0
        }
    }

    # ============================================================================
    # AOF Rewrite Tests
    # ============================================================================
    # Note: Full AOF round-trip tests (write → restart → load) are not included
    # because the test module registers classes dynamically via commands, which
    # creates a chicken-and-egg problem:
    # - Classes must be registered BEFORE AOF loading (in RedisModule_OnLoad)
    # - But the KEYMETA.REGISTER commands are in the AOF itself
    # - When server restarts and loads AOF, classes aren't registered yet
    # - KEYMETA.SET commands fail with "metadata class not found"
    #
    # For production modules, classes MUST be registered in RedisModule_OnLoad()
    # to ensure they're available when AOF/RDB files are loaded on server startup.
    # See src/module.c documentation for RM_CreateKeyMetaClass() for details.
    #
    # The test below verifies that AOF callbacks correctly emit KEYMETA.SET commands
    # to the AOF file during rewrite, which is the module's responsibility.
    test "KEYMETA - AOF rewrite emits correct KEYMETA.SET commands to file" {
        # This test verifies that the AOF callback implementation correctly writes
        # KEYMETA.SET commands to the AOF file during rewrite. We don't test the
        # full round-trip (restart + load) due to the dynamic registration limitation
        # explained above.

        r config set appendonly yes
        r config set auto-aof-rewrite-percentage 0
        r config set aof-use-rdb-preamble no
        # Wait for the initial AOF rewrite that Redis triggers when enabling AOF
        waitForBgrewriteaof r

        # Create keys with metadata from multiple classes
        r set key1 "value1"
        r keymeta.set [cname 1] key1 "metadata_c1"

        r set key2 "value2"
        r keymeta.set [cname 2] key2 "metadata_c2"
        r keymeta.set [cname 3] key2 "metadata_c3"

        r hset hashkey field1 val1
        r keymeta.set [cname 4] hashkey "hash_meta"

        # Trigger AOF rewrite
        r bgrewriteaof
        waitForBgrewriteaof r

        # Get the AOF directory and read the AOF file
        set aof_dir [lindex [r config get dir] 1]
        set aof_base_filename [lindex [r config get appendfilename] 1]

        # Find the base AOF file (after rewrite)
        set aof_files [glob -nocomplain -directory $aof_dir appendonlydir/${aof_base_filename}.*.base.aof]
        assert {[llength $aof_files] > 0}

        # Read the most recent base AOF file
        set aof_file [lindex [lsort $aof_files] end]
        set fp [open $aof_file r]
        set aof_content [read $fp]
        close $fp

        # Verify the AOF contains KEYMETA.SET commands with correct format
        assert_match "*KEYMETA.SET*[cname 1]*key1*metadata_c1*" $aof_content
        assert_match "*KEYMETA.SET*[cname 2]*key2*metadata_c2*" $aof_content
        assert_match "*KEYMETA.SET*[cname 3]*key2*metadata_c3*" $aof_content
        assert_match "*KEYMETA.SET*[cname 4]*hashkey*hash_meta*" $aof_content

        # Verify the RESP format is correct by checking for the command structure
        # The AOF should contain: *4 (array of 4 elements)
        assert_match "*\$11*KEYMETA.SET*" $aof_content
        # Count how many KEYMETA.SET commands are in the AOF
        set keymeta_count [regexp -all {KEYMETA\.SET} $aof_content]
        assert_equal $keymeta_count 4
    } {} {external:skip}

    # ========================================================================
    # RDB Save/Load Tests
    # ========================================================================

    test {RDB: SAVE and reload preserves metadata} {
        # Create key with metadata
        r set key1 "value1"
        r keymeta.set [cname 1] key1 "key1_meta1"
        assert_equal [r keymeta.get [cname 1] key1] "key1_meta1"

        r save
        r debug reload

        # Verify metadata persisted after reload
        assert_equal [r keymeta.get [cname 1] key1] "key1_meta1"

        flushallAndVerifyCleanup
    } {} {external:skip needs:save}

    test {RDB: BGSAVE writes metadata to RDB file} {
        # Create keys with different metadata combinations
        r set key1 "value1"
        r keymeta.set [cname 1] key1 "key1_meta1"

        r set key2 "value2"
        r keymeta.set [cname 1] key2 "key2_meta1"
        r keymeta.set [cname 2] key2 "key2_meta2"

        # Trigger BGSAVE and reload (debug reload preserves modules)
        r bgsave
        waitForBgsave r
        r debug reload

        # Verify metadata persisted after reload
        assert_equal [r keymeta.get [cname 1] key1] "key1_meta1"
        assert_equal [r keymeta.get [cname 1] key2] "key2_meta1"
        assert_equal [r keymeta.get [cname 2] key2] "key2_meta2"

        flushallAndVerifyCleanup
    } {} {external:skip needs:save}

    test {RDB: Metadata persists with expiretime} {
        # Create key with both expiry and metadata
        r set key1 "value1"
        set expire_time [expr {[clock seconds] + 10000}]
        r expireat key1 $expire_time
        r keymeta.set [cname 1] key1 "meta_with_expire"

        assert_equal [r expiretime key1] $expire_time
        assert_equal [r keymeta.get [cname 1] key1] "meta_with_expire"

        # Reload from RDB
        r debug reload

        # Verify metadata and expiry persist after reload
        assert_equal [r expiretime key1] $expire_time
        assert_equal [r keymeta.get [cname 1] key1] "meta_with_expire"

        flushallAndVerifyCleanup
    } {} {external:skip needs:debug}

    test {RDB: Create keys with upto 7 meta classes, with or without expiry} {
        # Test all combinations of 1-7 metadata classes, with or without expiry
        for {set n 1} {$n <= 7} {incr n} {
            foreach hasExpiry {0 1} {
                set keyname "key_${n}_exp${hasExpiry}"
                r set $keyname "value$n"

                # Set expiry if hasExpiry is 1
                if {$hasExpiry} {
                    set ttl [expr {3600 + $n}]
                    r expire $keyname $ttl
                    # Get the actual expiretime set by Redis to use as expected value
                    set expExpiry [r expiretime $keyname]
                }

                # Create list of class IDs to attach (1 through n)
                set class_ids {}
                for {set i 1} {$i <= $n} {incr i} {
                    lappend class_ids $i
                }

                # Randomize the order of metadata attachment
                set class_ids [lshuffle $class_ids]

                # Attach metadata in randomized order
                foreach cid $class_ids {
                    r keymeta.set [cname $cid] $keyname "meta$cid"
                }

                # Verify metadata before RDB save
                # Verify exactly n metadata classes are attached
                for {set i 1} {$i <= 7} {incr i} {
                    if {$i <= $n} {
                        assert_equal [r keymeta.get [cname $i] $keyname] "meta$i"
                    } else {
                        assert_equal [r keymeta.get [cname $i] $keyname] ""
                    }
                }

                # Verify expiry before RDB save
                if {$hasExpiry} {
                    set actual_expiretime [r expiretime $keyname]
                    assert_equal $actual_expiretime $expExpiry
                }

                # Save and reload from RDB (debug reload preserves modules)
                r save
                r debug reload

                # Verify metadata after RDB reload
                # Verify exactly n metadata classes are still attached
                for {set i 1} {$i <= 7} {incr i} {
                    if {$i <= $n} {
                        assert_equal [r keymeta.get [cname $i] $keyname] "meta$i"
                    } else {
                        assert_equal [r keymeta.get [cname $i] $keyname] ""
                    }
                }

                # Verify expiry after RDB reload
                if {$hasExpiry} {
                    set actual_expiretime [r expiretime $keyname]
                    assert_equal $actual_expiretime $expExpiry
                } else {
                    # Verify no expiry set
                    assert_equal [r expiretime $keyname] -1
                }
                flushallAndVerifyCleanup
            }
        }
    } {} {external:skip needs:save}

    # ========================================================================
    # RDB Flag Tests: ALLOW_IGNORE, RDBLOAD, RDBSAVE
    # ========================================================================

    # Test all combinations except the error case (ALLOW_IGNORE=0, RDBLOAD=0, RDBSAVE=1)
    foreach RDBLOAD {0 1} {
        foreach RDBSAVE {0 1} {
            foreach ALLOW_IGNORE {0 1} {
                # Skip the error case - we'll test it last since it causes RDB load to fail
                if {!$RDBLOAD && $RDBSAVE && !$ALLOW_IGNORE} { continue }

                test "RDB: SAVE and LOAD (ALLOW_IGNORE=$ALLOW_IGNORE, RDBLOAD=$RDBLOAD, RDBSAVE=$RDBSAVE)" {
                    # Flush all data and save empty RDB to start with a clean slate
                    r flushall
                    r save

                    # re-register class 1 with new flags. Expected re-registered same class ID
                    r keymeta.unregister [cname 1]
                    # dummy default spec
                    set newSpec "KEEPONCOPY"
                    if {$ALLOW_IGNORE} { append newSpec ":ALLOWIGNORE" }
                    if {$RDBLOAD} { append newSpec ":RDBLOAD" }
                    if {$RDBSAVE} { append newSpec ":RDBSAVE" }

                    # Must reuse same class-id that it had before
                    assert_equal $classes(1) [r keymeta.register [cname 1] 1 $newSpec]

                    r set key1 "value1"
                    r keymeta.set [cname 1] key1 "key1_meta1"
                    assert_equal [r keymeta.get [cname 1] key1] "key1_meta1"

                    r save
                    r debug reload

                    # Metadata is preserved only when BOTH rdb_save AND rdb_load are enabled
                    # Otherwise metadata is lost (either not saved, or saved but not loaded)
                    set metaPreserved [expr {$RDBSAVE && $RDBLOAD}]
                    set expectedMeta [expr {$metaPreserved ? "key1_meta1" : ""}]

                    assert_equal [r keymeta.get [cname 1] key1] $expectedMeta

                    flushallAndVerifyCleanup
                } {} {external:skip needs:save}
            }
        }
    }

    # Test the error case last (ALLOW_IGNORE=0, RDBLOAD=0, RDBSAVE=1)
    # This test causes RDB load to fail, so we test it last to avoid polluting subsequent tests
    test "RDB: SAVE and LOAD Invalid combination: (ALLOW_IGNORE=0, RDBLOAD=0, RDBSAVE=1)" {
        # re-register class 1 with RDBSAVE flag but no RDBLOAD or ALLOW_IGNORE
        r keymeta.unregister [cname 1]
        set newSpec "KEEPONCOPY:RDBSAVE"
        assert_equal $classes(1) [r keymeta.register [cname 1] 1 $newSpec]

        r set key1 "value1"
        r keymeta.set [cname 1] key1 "key1_meta1"
        assert_equal [r keymeta.get [cname 1] key1] "key1_meta1"

        r save

        # This combination causes RDB load to fail because:
        # - Metadata was saved (RDBSAVE=1)
        # - Class has no rdb_load callback (RDBLOAD=0)
        # - Errors are not ignored (ALLOW_IGNORE=0)
        catch {r debug reload} err
        assert_match "*Error trying to load the RDB dump*" $err
    } {} {external:skip needs:save}

    # ========================================================================
    # DUMP/RESTORE Tests
    # ========================================================================

    test {DUMP/RESTORE: 1 to 7 metadata classes, optional TTL} {
        foreach withTTL {0 1} {
            for {set numClasses 1} {$numClasses < 8} {incr numClasses} {
                # Re-register classes with RDBLOAD and RDBSAVE flags
                for {set cid 1} {$cid <= $numClasses} {incr cid} {
                    r keymeta.unregister [cname $cid]
                    assert_equal $classes($cid) [r keymeta.register [cname $cid] 1 $classesSpec($cid)]
                }

                # Create key with metadata classes
                r set key1 "value1"
                for {set i 1} {$i <= $numClasses} {incr i} {
                    r keymeta.set [cname $i] key1 "meta${i}_value"
                }

                if {$withTTL} { r expire key1 10000 }

                # Verify all metadata before DUMP
                for {set i 1} {$i <= $numClasses} {incr i} {
                    assert_equal [r keymeta.get [cname $i] key1] "meta${i}_value"
                }

                # DUMP the key
                set encoded [r dump key1]

                # Delete and RESTORE
                r del key1
                r restore key1 [expr {$withTTL ? 10000 : 0}] $encoded

                # Verify all metadata was restored
                assert_equal [r get key1] "value1"
                for {set i 1} {$i <= $numClasses} {incr i} {
                    assert_equal [r keymeta.get [cname $i] key1] "meta${i}_value"
                }
                if {$withTTL} { assert_range [r pttl key1] 9000 10000 }

                flushallAndVerifyCleanup
            }
        }
    }

    test {DUMP/RESTORE: REPLACE with metadata} {
        # Create key with metadata
        r set key1 value1
        r keymeta.set [cname 1] key1 "meta1_original"

        # DUMP the key
        set encoded1 [r dump key1]

        # Create different key with different metadata
        r set key1 value2
        r keymeta.set [cname 1] key1 "meta1_new"

        # DUMP the second version
        set encoded2 [r dump key1]

        # Delete and restore first version
        r del key1
        r restore key1 0 $encoded1
        assert_equal [r get key1] "value1"
        assert_equal [r keymeta.get [cname 1] key1] "meta1_original"

        # RESTORE second version with REPLACE
        r restore key1 0 $encoded2 replace
        assert_equal [r get key1] "value2"
        assert_equal [r keymeta.get [cname 1] key1] "meta1_new"

        flushallAndVerifyCleanup
    }


    # Test all combinations except the error case (ALLOW_IGNORE=0, RDBLOAD=0, RDBSAVE=1)
    foreach RDBLOAD {0 1} {
        foreach RDBSAVE {0 1} {
            foreach ALLOW_IGNORE {0 1} {
                # Skip the error case - we'll test it last since it causes RESTORE to fail
                if {!$RDBLOAD && $RDBSAVE && !$ALLOW_IGNORE} { continue }

                test "DUMP/RESTORE: (ALLOW_IGNORE=$ALLOW_IGNORE, RDBLOAD=$RDBLOAD, RDBSAVE=$RDBSAVE)" {
                    # re-register class 1 with new flags. Expected re-registered same class ID
                    r keymeta.unregister [cname 1]
                    # dummy default spec
                    set newSpec "KEEPONCOPY"
                    if {$ALLOW_IGNORE} { append newSpec ":ALLOWIGNORE" }
                    if {$RDBLOAD} { append newSpec ":RDBLOAD" }
                    if {$RDBSAVE} { append newSpec ":RDBSAVE" }

                    # Must reuse same class-id that it had before
                    assert_equal $classes(1) [r keymeta.register [cname 1] 1 $newSpec]

                    r set key1 "value1"
                    r keymeta.set [cname 1] key1 "key1_meta1"
                    assert_equal [r keymeta.get [cname 1] key1] "key1_meta1"

                    # DUMP & RESTORE
                    set encoded [r dump key1]
                    r del key1
                    r restore key1 0 $encoded

                    # Metadata is preserved only when BOTH rdb_save AND rdb_load are enabled
                    # Otherwise metadata is lost (either not saved, or saved but not loaded)
                    set metaPreserved [expr {$RDBSAVE && $RDBLOAD}]
                    set expectedMeta [expr {$metaPreserved ? "key1_meta1" : ""}]

                    assert_equal [r keymeta.get [cname 1] key1] $expectedMeta

                    flushallAndVerifyCleanup
                }
            }
        }
    }

    # Test the error case last (ALLOW_IGNORE=0, RDBLOAD=0, RDBSAVE=1)
    # This test causes RESTORE to fail, so we test it last to avoid polluting subsequent tests
    test "DUMP/RESTORE: Invalid combination: (ALLOW_IGNORE=0, RDBLOAD=0, RDBSAVE=1)" {
        # re-register class 1 with RDBSAVE flag but no RDBLOAD or ALLOW_IGNORE
        r keymeta.unregister [cname 1]
        set newSpec "KEEPONCOPY:RDBSAVE"
        assert_equal $classes(1) [r keymeta.register [cname 1] 1 $newSpec]

        r set key1 "value1"
        r keymeta.set [cname 1] key1 "key1_meta1"
        assert_equal [r keymeta.get [cname 1] key1] "key1_meta1"

        # DUMP the key
        set encoded [r dump key1]

        # Delete and try to RESTORE
        r del key1

        # This combination causes RESTORE to fail because:
        # - Metadata was saved (RDBSAVE=1)
        # - Class has no rdb_load callback (RDBLOAD=0)
        # - Errors are not ignored (ALLOW_IGNORE=0)
        catch {r restore key1 0 $encoded} err
        assert_match "*Bad data format*" $err

        flushallAndVerifyCleanup
    }
}

test "RDB: Load with different module registration order preserves metadata correctly" {
    # This test verifies out-of-order metadata attachment during RDB load.
    # When modules register in different order at load time vs save time,
    # metadata values should still be correctly associated with their classes.
    start_server {tags {"modules" "external:skip" "cluster:skip"} overrides {enable-debug-command yes}} {
        r module load $testmodule

        # Helper function to generate class names (needed in inner scope)
        proc cname {id} { return "CLS$id" }

        # Register classes in order: 1, 2, 3
        set spec1 "KEEPONCOPY:ALLOWIGNORE:RDBLOAD:RDBSAVE"
        set spec2 "KEEPONRENAME:ALLOWIGNORE:RDBLOAD:RDBSAVE"
        set spec3 "KEEPONMOVE:ALLOWIGNORE:RDBLOAD:RDBSAVE"

        set class1 [r keymeta.register [cname 1] 1 $spec1]
        set class2 [r keymeta.register [cname 2] 1 $spec2]
        set class3 [r keymeta.register [cname 3] 1 $spec3]

        # Verify class IDs match registration order
        assert_equal $class1 1 "Class 1 registered first, gets ID 1"
        assert_equal $class2 2 "Class 2 registered second, gets ID 2"
        assert_equal $class3 3 "Class 3 registered third, gets ID 3"

        # OUTER SERVER: Create RDB with classes registered in order 1,2,3
        r flushall
        r set mykey "myvalue"
        r keymeta.set [cname 1] mykey "metadata_for_class1"
        r keymeta.set [cname 2] mykey "metadata_for_class2"
        r keymeta.set [cname 3] mykey "metadata_for_class3"

        # Verify metadata before save
        assert_equal [r keymeta.get [cname 1] mykey] "metadata_for_class1"
        assert_equal [r keymeta.get [cname 2] mykey] "metadata_for_class2"
        assert_equal [r keymeta.get [cname 3] mykey] "metadata_for_class3"

        r save

        # Get RDB file path & Copy RDB to a temp location with unique name
        set rdb_dir [lindex [r config get dir] 1]
        set rdb_file [lindex [r config get dbfilename] 1]
        set rdb_path [file join $rdb_dir $rdb_file]
        set temp_rdb [file join $rdb_dir "temp_metadata_outoforder_[pid].rdb"]
        file copy -force $rdb_path $temp_rdb

        # INNER SERVER: Start new server, register classes in DIFFERENT order, then load RDB
        start_server [list overrides [list dir $rdb_dir enable-debug-command yes]] {
            r module load $testmodule

            # Helper function to generate class names (needed in inner scope)
            proc cname {id} { return "CLS$id" }

            # Register classes in DIFFERENT order: 3, 1, 2
            # This simulates a server where modules load in different order
            set spec1 "KEEPONCOPY:ALLOWIGNORE:RDBLOAD:RDBSAVE"
            set spec2 "KEEPONRENAME:ALLOWIGNORE:RDBLOAD:RDBSAVE"
            set spec3 "KEEPONMOVE:ALLOWIGNORE:RDBLOAD:RDBSAVE"

            set class3 [r keymeta.register [cname 3] 1 $spec3]
            set class1 [r keymeta.register [cname 1] 1 $spec1]
            set class2 [r keymeta.register [cname 2] 1 $spec2]

            # Verify class IDs are assigned by REGISTRATION ORDER, not name
            # We registered in order 3,1,2, so the runtime IDs are:
            # - class3 (name "CLS3") gets ID 1 (first registered)
            # - class1 (name "CLS1") gets ID 2 (second registered)
            # - class2 (name "CLS2") gets ID 3 (third registered)
            # This is DIFFERENT from outer server which registered in order 1,2,3
            assert_equal $class3 1 "Class 3 registered first, gets ID 1"
            assert_equal $class1 2 "Class 1 registered second, gets ID 2"
            assert_equal $class2 3 "Class 2 registered third, gets ID 3"

            # Copy the saved RDB to this server's dbfilename
            set inner_rdb_file [lindex [r config get dbfilename] 1]
            set inner_rdb_path [file join $rdb_dir $inner_rdb_file]
            file copy -force $temp_rdb $inner_rdb_path

            # NOW load the RDB (AFTER registration in different order)
            # Use 'nosave' to reload from the copied RDB without saving current state first
            r debug reload nosave

            # Verify the key exists
            assert_equal [r exists mykey] 1 "Key should exist after RDB load"
            assert_equal [r get mykey] "myvalue" "Key value should be preserved"

            # Verify metadata values are correctly associated with their classes
            # WITHOUT metadata would be swapped because:
            # - At SAVE time (outer): classes registered in order 1,2,3
            # - At LOAD time (inner): classes registered in order 3,1,2
            # - RDB contains metadata in saved order, but keyMetaClassLookupByName
            #   maps them back to correct classes by NAME, not by registration order
            assert_equal [r keymeta.get [cname 1] mykey] "metadata_for_class1"
            assert_equal [r keymeta.get [cname 2] mykey] "metadata_for_class2"
            assert_equal [r keymeta.get [cname 3] mykey] "metadata_for_class3"
        }

        # Cleanup temp file
        file delete $temp_rdb

    }
} {} {external:skip needs:save}

test "RDB: File size same with/without metadata when no rdb_save callback" {
    # This test verifies that when a metadata class has no rdb_save callback,
    # the metadata is not serialized to RDB, so the RDB file size should be
    # approximately the same (within a small tolerance for header differences).

    start_server {tags {"modules" "external:skip" "cluster:skip"} overrides {enable-debug-command yes}} {
        r module load $testmodule

        # Get RDB directory
        set rdb_dir [lindex [r config get dir] 1]
        set rdb_file [lindex [r config get dbfilename] 1]
        set rdb_path [file join $rdb_dir $rdb_file]

        # Test 1: Create key WITHOUT metadata and save
        r flushall
        r set key1 "test_value_12345"
        r save
        set size_without_meta [file size $rdb_path]
        
        # Test 2: Create identical key WITH metadata (but no rdb_save) and save        
        # Register a class WITHOUT rdb_save callback (RDBSAVE=0)
        # Use ALLOWIGNORE so loading doesn't fail when metadata is missing
        set spec "ALLOWIGNORE"
        r keymeta.register [cname 1] 1 $spec
        
        r flushall
        r set key1 "test_value_12345"
        r keymeta.set [cname 1] key1 "some_metadata_value"

        # Verify metadata is attached
        assert_equal [r keymeta.get [cname 1] key1] "some_metadata_value"

        r save
        set size_with_meta [file size $rdb_path]

        # The file sizes should be the same (metadata not serialized)
        assert_equal $size_without_meta $size_with_meta
    }
} {} {external:skip needs:save}

test "Creating key metadata not during OnLoad should fail" {
    # This time start_server without "enable-debug-command yes"
    start_server {tags {"modules" "external:skip" "cluster:skip"} overrides {enable-debug-command no}} {
        r module load $testmodule
        # Creating a class not during OnLoad should fail
        catch {r keymeta.register [cname 1] 1 "ALLOWIGNORE"} err
        assert_match {*failed to create metadata class*} $err        
    }
} {} {external:skip needs:save}
