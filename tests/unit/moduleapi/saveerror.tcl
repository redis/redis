set testmodule [file normalize tests/modules/saveerror.so]

# Regression test for RM_SaveDataTypeToString error path.
# Verifies the internal sds buffer is properly freed when io.error is set.
# Run with address sanitizer to confirm no memory leaks:
#   make SANITIZER=address
#   ./runtest --single unit/moduleapi/saveerror

start_server {tags {"modules external:skip"}} {
    r module load $testmodule

    test {SaveDataTypeToString handles io.error without leaking} {
        r saveerror.set mykey 42
        # Triggers RM_SaveDataTypeToString error path
        catch {r saveerror.dump mykey} e
        assert_equal $e "ERR io error"
    }
}
