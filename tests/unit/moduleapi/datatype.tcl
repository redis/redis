set testmodule [file normalize tests/modules/datatype.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {DataType: Test module is sane, GET/SET work.} {
        r datatype.set dtkey 100 stringval
        assert {[r datatype.get dtkey] eq {100 stringval}}
    }

    test {DataType: RM_SaveDataTypeToString(), RM_LoadDataTypeFromString() work} {
        r datatype.set dtkey -1111 MyString
        set encoded [r datatype.dump dtkey]

        r datatype.restore dtkeycopy $encoded
        assert {[r datatype.get dtkeycopy] eq {-1111 MyString}}
    }

    test {DataType: Handle truncated RM_LoadDataTypeFromString()} {
        r datatype.set dtkey -1111 MyString
        set encoded [r datatype.dump dtkey]
        set truncated [string range $encoded 0 end-1]

        catch {r datatype.restore dtkeycopy $truncated} e
        set e
    } {*Invalid*}
}
