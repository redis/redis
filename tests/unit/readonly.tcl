start_server {tags {"readonly"}} {
    test {explicitly set config readonly off and SET and GET an empty item} {
        r config set readonly no
        r get rok
        r set rok value
        r get rok
    } {value}

    test {explicitly set config readonly on and GET an item} {
        r config set readonly no
        r set rok value
        r get rok
        r config set readonly yes
        r get rok
    } {value}

    test {explicitly set config readonly on and SET and GET an empty item} {
        r config set readonly no
        r set rok value1
        r config set readonly yes
        assert_error ERR*readonly* {r set key value}
    } {}
}
