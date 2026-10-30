start_server {} {
    set i [r info]
    regexp {redis_version:(.*?)\r\n} $i - version
    regexp {redis_git_sha1:(.*?)\r\n} $i - sha1
    puts "Testing Redis version $version ($sha1)"

    test "redis-server --version --short" {
        set short_version [string trim [exec src/redis-server --version --short]]
        assert_equal $version $short_version
    }
}
