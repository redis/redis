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

    test {DataType: ModuleTypeReplaceValue() happy path works} {
        r datatype.set key-a 1 AAA
        r datatype.set key-b 2 BBB

        assert {[r datatype.swap key-a key-b] eq {OK}}
        assert {[r datatype.get key-a] eq {2 BBB}}
        assert {[r datatype.get key-b] eq {1 AAA}}
    }

    test {DataType: ModuleTypeReplaceValue() fails on non-module keys} {
        r datatype.set key-a 1 AAA
        r set key-b RedisString

        catch {r datatype.swap key-a key-b} e
        set e
    } {*ERR*}
}
