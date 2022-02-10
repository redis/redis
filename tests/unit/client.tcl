start_server {tags {"Client Caching"}} {
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

start_server {tags {"Client Kill"}} {
    test {CLIENT kill wrong number of arguments} {
        catch {r client kill} err
        set _ $err
    } {ERR*wrong number of arguments*}

    test {CLIENT kill wrong address} {
        catch {r client kill 000.123.321.567:0000} err
        set _ $err
    } {ERR*No such*}

    test {CLIENT kill no port} {
        catch {r client kill 127.0.0.1:} err
        set _ $err
    } {ERR*No such*}
}

start_server {tags {"Client No-evict"}} {
    test {CLIENT no-evict wrong number of arguments} {
        catch {r client no-evict ok x} err
        set _ $err
    } {ERR*wrong number of arguments*}

    test {CLIENT no-evict wrong argument} {
        catch {r client no-evict wrongInput} err
        set _ $err
    } {ERR*syntax*}
}


start_server {tags {"Client pause"}} {
    test {CLIENT pause wrong number of arguments} {
        catch {r client pause} err
        set _ $err
    } {ERR*wrong number of arguments*}

    test {CLIENT pause wrong timeout type} {
        catch {r client pause abc} err
        set _ $err
    } {ERR*timeout is not an integer*}

    test {CLIENT pause negative timeout} {
        catch {r client pause -1} err
        set _ $err
    } {ERR timeout is negative}
}

start_server {tags {"Client reply"}} {
    test {CLIENT reply wrong number of arguments} {
        catch {r client reply on x} err
        set _ $err
    } {ERR*wrong number of arguments*}

    test {CLIENT reply wrong argument} {
        catch {r client reply wrongInput} err
        set _ $err
    } {ERR*syntax*}
}

start_server {tags {"Client tracking"}} {
    test {CLIENT tracking wrong number of arguments} {
        catch {r client tracking} err
        set _ $err
    } {ERR*wrong number of arguments*}

    test {CLIENT tracking wrong argument} {
        catch {r client tracking wrongInput} err
        set _ $err
    } {ERR*syntax*}

    test {CLIENT tracking wrong option} {
        catch {r client tracking on wrongInput} err
        set _ $err
    } {ERR*syntax*}
}

start_server {tags {"Client setname getname"}} {
    test {CLIENT setname wrong number of arguments} {
        catch {r client setname on x} err
        set _ $err
    } {ERR*wrong number of arguments*}

    test {CLIENT getname check if name set correctly} {
        r client setname testName
        r client getName
    } {testName}
}

start_server {tags {"Client _ wrong arguments"}} {
    test {CLIENT getredir wrong number of arguments} {
        catch {r client getredir x} err
        set _ $err
    } {ERR*wrong number of arguments*}

    test {CLIENT id wrong number of arguments} {
        catch {r client id x} err
        set _ $err
    } {ERR*wrong number of arguments*}

    test {CLIENT info wrong number of arguments} {
        catch {r client info x} err
        set _ $err
    } {ERR*wrong number of arguments*}

    test {CLIENT info wrong number of arguments} {
        catch {r client info x} err
        set _ $err
    } {ERR*wrong number of arguments*}

    test {CLIENT list wrong number of arguments} {
        catch {r client list x} err
        set _ $err
    } {ERR*syntax*}

    test {CLIENT trackinginfo wrong number of arguments} {
        catch {r client trackinginfo x} err
        set _ $err
    } {ERR*wrong number of arguments*}

    test {CLIENT unpause wrong number of arguments} {
        catch {r client unpause x} err
        set _ $err
    } {ERR*wrong number of arguments*}
}