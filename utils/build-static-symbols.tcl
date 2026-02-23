# Build a symbol table for static symbols of redis.c
# Useful to get stack traces on segfault without a debugger. See redis.c
# for more information.
#
# Copyright(C) 2009-Present Redis Ltd. All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).

set fd [open redis.c]
set symlist {}
while {[gets $fd line] != -1} {
    if {[regexp {^static +[A-z0-9]+[ *]+([A-z0-9]*)\(} $line - sym]} {
        lappend symlist $sym
    }
}
set symlist [lsort -unique $symlist]
puts "static struct redisFunctionSym symsTable\[\] = {"
foreach sym $symlist {
    puts "{\"$sym\",(unsigned long)$sym},"
}
puts "{NULL,0}"
puts "};"

close $fd
