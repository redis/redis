start_server {tags {"bfilter"}} {
    test {BFCREATE create a Bloomfilter value} {
        r bfcreate bf 100 3
        r exists bf
    } {1}

    test {Corrupted create an existing Bloomfilter} {
        catch {r bfcreate bf 100 3} e
        set e
    } {*filter object is already exist*}

    test {BFMATCH exists element} {
        r bfadd bf 1 2 3
        r bfmatch bf 1 2 3
    } {1 1 1}

    test {BFMATCH not exists element} {
        r bfmatch bf 4 5 6
    } {0 0 0}

    test {BFADD add empty string} {
        r bfadd bf ""
    } {OK}

    test {Corrupted BFADD when key not exists} {
        set e {}
        catch {r bfadd bf1 ""} e
        set e
    } {*no such key*}

    test {create a most largest Bloomfilter} {
        r del bf
        r bfcreate bf 4294967168 3
    } {OK}

    test {Corrupted create a Bloomfilter beyond maximum allowed size} {
        r del bf
        set e {}
        catch {r bfcreate bf 4294967169 3} e
        set e
    } {*string exceeds maximum allowed size*}
}
