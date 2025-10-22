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
# - LAZYFREE (TBD) 
# ============================================================================

set testmodule [file normalize tests/modules/test_keymeta.so]

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
        r keymeta.set $keyname $i "blabla$i"
        assert_equal [r keymeta.get $keyname $i] "blabla$i"
        r keymeta.set $keyname $i "meta$i"
    }

    # Verify metadata was set correctly
    for {set i 1} {$i <= $numClasses} {incr i} {
        assert_equal [r keymeta.get $keyname $i] "meta$i"
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
        assert_equal [r keymeta.get $keyname $i] $expected
    }
}

proc flushallAndVerifyCleanup {} {
    r flushall
    # Verify all metadata is cleaned up properly    
    assert_equal [r keymeta.active] 0
}

start_server {tags {"modules" "external:skip" "cluster:skip"}} {
    r module load $testmodule

    array set classesSpec {}
    set classesSpec(1) "KEEPONCOPY:KEEPONRENAME:KEEPONMOVE"
    set classesSpec(2) "KEEPONCOPY:KEEPONRENAME:UNLINKFREE"
    set classesSpec(3) "KEEPONCOPY"
    set classesSpec(4) ""
    set classesSpec(5) "KEEPONRENAME:KEEPONMOVE"
    set classesSpec(6) "KEEPONRENAME"
    set classesSpec(7) "KEEPONMOVE:UNLINKFREE"

    array set classes {}
    for {set cid 1} {$cid <= 7} {incr cid} {
        set spec $classesSpec($cid)
        set classes($cid) [r keymeta.register XXXXXCID$cid 1 $spec]
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
            r keymeta.set k1 $cid "meta1"
            assert_equal [r keymeta.active] [incr numAlloc]
            r keymeta.set k1 $cid "meta1b"
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
            r keymeta.set k1 $cid "meta1"
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
            r keymeta.set k1 $cid "meta1"
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
            r keymeta.set myset $cid "meta"
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
            r keymeta.set mykey $cid "meta"
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
}
