start_server {} {

    test {check swap_inprogress_count after swapping not exist keys} {
        assert_equal [getInfoProperty [{*}r info swap] swap_inprogress_count] 0
        r hset h1 k1 v1 k2 v2 k3 v3
        assert_equal [getInfoProperty [{*}r info swap] swap_inprogress_count] 0
    }

}
