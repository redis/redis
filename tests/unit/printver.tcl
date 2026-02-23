start_server {} {
    set i [r info]
    regexp {redis_version:(.*?)\r\n} $i - version
    regexp {redis_git_sha1:(.*?)\r\n} $i - sha1
    puts "Testing Redis version $version ($sha1)"
}
