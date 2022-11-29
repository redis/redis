start_server {tags {"auth control"} overrides {auth-threshold 3 requirepass foobar}} {
    test {AUTH succeeds when the right password is given} {
        r auth foobar
    } {OK}

    test {AUTH fails when a wrong password is given} {
        catch {r auth wrong!} err
        set _ $err
    } {WRONGPASS*}

    test {turn on auth control} {
        catch {r auth wrong!} err
        assert_match {WRONGPASS*} $err

        catch {r auth wrong!} err
        assert_match {WRONGPASS*} $err

        catch {r auth wrong!} err
        assert_match {I/O error reading reply} $err

        catch {r auth foobar} err
        assert_match {OK} $err

        catch {r auth wrong!} err
        assert_match {I/O error reading reply} $err

        catch {r auth wrong!} err
        assert_match {I/O error reading reply} $err

        after 1000
        catch {r auth wrong!} err
        assert_match {WRONGPASS*} $err
    }
}

start_server {tags {"auth control"} overrides {requirepass foobar}} {
    test {turn off auth control} {
        catch {r auth foobar} err
        assert_match {OK} $err

        catch {r auth wrong!} err
        assert_match {WRONGPASS*} $err

        catch {r auth wrong!} err
        assert_match {WRONGPASS*} $err

        catch {r auth wrong!} err
        assert_match {WRONGPASS*} $err

        catch {r auth wrong!} err
        assert_match {WRONGPASS*} $err

        catch {r auth foobar} err
        assert_match {OK} $err

        catch {r auth wrong!} err
        assert_match {WRONGPASS*} $err
    }
}
