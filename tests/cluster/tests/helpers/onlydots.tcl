# Read the standard input and only shows dots in the output, filtering out
# all the other characters. Designed to avoid bufferization so that when
# we get the output of redis-trib and want to show just the dots, we'll see
# the dots as soon as redis-trib will output them.

fconfigure stdin -buffering none

while 1 {
    set c [read stdin 1]
    if {$c eq {}} {
        exit 0; # EOF
    } elseif {$c eq {.}} {
        puts -nonewline .
        flush stdout
    }
}
