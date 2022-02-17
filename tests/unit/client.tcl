start_server {} {
    test {CLIENT Caching wrong number of arguments} {
        catch {r client caching} err
        set _ $err
    } {ERR*wrong number of arguments*}

    test {CLIENT Caching wrong argument} {
        catch {r client caching maybe} err
        set _ $err
    } {ERR*when the client is in tracking mode*}

    test {CLIENT Caching OFF without optout} {
        catch {r client caching off} err
        set _ $err
    } {ERR*when the client is in tracking mode*}

    test {CLIENT Caching ON without optin} {
        catch {r client caching on} err
        set _ $err
    } {ERR*when the client is in tracking mode*}

    test {CLIENT Caching ON with optout} {
        r CLIENT TRACKING ON optout 
        catch {r client caching on} err
        set _ $err
    } {ERR*syntax*}
    
    test {CLIENT Caching OFF with optin} {
        r CLIENT TRACKING off optout 
        catch {r client caching on} err
        set _ $err
    } {ERR*when the client is in tracking mode*}
}

start_server {} {
    test {CLIENT kill wrong address} {
        catch {r client kill 000.123.321.567:0000} err
        set _ $err
    } {ERR*No such*}

    test {CLIENT kill no port} {
        catch {r client kill 127.0.0.1:} err
        set _ $err
    } {ERR*No such*}
}

start_server {} {
    test {CLIENT no-evict wrong argument} {
        catch {r client no-evict wrongInput} err
        set _ $err
    } {ERR*syntax*}
}


start_server {} {
    test {CLIENT pause wrong timeout type} {
        catch {r client pause abc} err
        set _ $err
    } {ERR*timeout is not an integer*}

    test {CLIENT pause negative timeout} {
        catch {r client pause -1} err
        set _ $err
    } {ERR timeout is negative}
}

start_server {} {
    test {CLIENT reply wrong argument} {
        catch {r client reply wrongInput} err
        set _ $err
    } {ERR*syntax*}
}

start_server {} {

    test {CLIENT tracking wrong argument} {
        catch {r client tracking wrongInput} err
        set _ $err
    } {ERR*syntax*}

    test {CLIENT tracking wrong option} {
        catch {r client tracking on wrongInput} err
        set _ $err
    } {ERR*syntax*}
}

start_server {} {
    test {CLIENT getname check if name set correctly} {
        r client setname testName
        r client getName
    } {testName}
}
