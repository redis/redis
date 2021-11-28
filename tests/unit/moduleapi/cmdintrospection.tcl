set testmodule [file normalize tests/modules/cmdintrospection.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module command introspection via COMMAND" {
        set reply [r command info cmdintrospection.xadd]
        assert_equal $reply {{cmdintrospection.xadd -5 {} 1 1 1 {} {summary {Appends a new entry to a stream} complexity {O(1) when adding a new entry, O(N) when trimming where N being the number of entries evicted.} since 5.0.0 group module history {{6.2 {Added the NOMKSTREAM option, MINID trimming strategy and the LIMIT option}}} hints {hint1 hint2 hint3} arguments {{name key type key value key} {name nomkstream type pure-token token NOMKSTREAM flags optional} {name trimming type block flags optional value {{name trim_startegy type oneof value {{name maxlen type pure-token token MAXLEN} {name minid type pure-token token MINID since 6.2.0}}} {name trim_op type oneof flags optional value {{name exact type pure-token token =} {name approx type pure-token token ~}}} {name trim_threshold type string value threshold} {name trim_count type integer token LIMIT since 6.2.0 flags optional value count}}} {name id type oneof value {{name id_auto type pure-token token *} {name id_given type string value id}}} {name fields_and_values type block flags multiple value {{name field type string value field} {name value type string value value}}}} key-specs {{flags write begin-search {type index spec {index 1}} find-keys {type range spec {lastkey 0 keystep 1 limit 0}}}}}}}
    }
}
