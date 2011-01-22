# -*- sh -*-
#
# Bash completion function for the 'redis-cli' command.
#
# Steve
# --
# http://www.steve.org.uk
#

_redis-cli()
{
    COMPREPLY=()
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}

    #
    #  All known commands accepted.  Sorted.
    #
    opts='bgrewriteaof bgsave dbsize debug decr decrby del echo exists expire expireat flushall flushdb get getset incr incrby info keys lastsave lindex llen lpop lpush lrange lrem lset ltrim mget move mset msetnx ping randomkey rename renamenx rewriteaof rpop rpoplpush rpush sadd save scard sdiff sdiffstore select set setnx shutdown sinter sinterstore sismember slaveof smembers smove sort spop srandmember srem sunion sunionstore ttl type zadd zcard zincrby zrange zrangebyscore zrem zremrangebyscore zrevrange zscore'

    #
    #  Only complete on the first term.
    #
    if [ $COMP_CWORD -eq 1 ]; then
        COMPREPLY=( $(compgen -W "${opts}" -- ${cur}) )
        return 0
    fi

}
complete -F _redis-cli redis-cli
