-module(erldis).

-compile(export_all).
-define(EOL, "\r\n").

%% helpers
flatten({error, Message}) ->
    {error, Message};
flatten(List) when is_list(List)->   
    lists:flatten(List).

%% exposed API
connect(Host) ->
    client:connect(Host).

quit(Client) ->
    client:asend(Client, "QUIT"),
    client:disconnect(Client).

%% Commands operating on string values
internal_set_like(Client, Command, Key, Value) ->
    client:send(Client, Command, [[Key, length(Value)],
                                  [Value]]).

get_all_results(Client) -> client:get_all_results(Client).

auth(Client, Password) -> client:ssend(Client, auth, [Password]).

set(Client, Key, Value) -> internal_set_like(Client, set, Key, Value).
get(Client, Key) -> client:ssend(Client, get, [Key]).
getset(Client, Key, Value) -> internal_set_like(Client, getset, Key, Value).
mget(Client, Keys) -> client:ssend(Client, mget, Keys).
setnx(Client, Key, Value) -> internal_set_like(Client, setnx, Key, Value).
incr(Client, Key) -> client:ssend(Client, incr, [Key]).
incrby(Client, Key, By) -> client:ssend(Client, incrby, [Key, By]).
decr(Client, Key) -> client:ssend(Client, decr, [Key]).
decrby(Client, Key, By) -> client:ssend(Client, decrby, [Key, By]).



%% Commands operating on every value
exists(Client, Key) -> client:ssend(Client, exists, [Key]).
del(Client, Key) -> client:ssend(Client, del, [Key]).
type(Client, Key) -> client:ssend(Client, type, [Key]).
keys(Client, Pattern) -> client:ssend(Client, keys, [Pattern]).
randomkey(Client, Key) -> client:ssend(Client, randomkey, [Key]).
rename(Client, OldKey, NewKey) -> client:ssend(Client, rename, [OldKey, NewKey]).
renamenx(Client, OldKey, NewKey) -> client:ssend(Client, renamenx, [OldKey, NewKey]).
dbsize(Client) -> client:ssend(Client, dbsize).
expire(Client, Key, Seconds) -> client:ssend(Client, expire, [Key, Seconds]).
ttl(Client, Key) -> client:ssend(Client, ttl, [Key]).



%% Commands operating on lists
rpush(Client, Key, Value) -> internal_set_like(Client, rpush, Key, Value).
lpush(Client, Key, Value) -> internal_set_like(Client, lpush, Key, Value).
llen(Client, Key) -> client:ssend(Client, llen, [Key]).
lrange(Client, Key, Start, End) -> client:ssend(Client, lrange, [Key, Start, End]).
ltrim(Client, Key, Start, End) -> client:ssend(Client, ltrim, [Key, Start, End]).
lindex(Client, Key, Index) -> client:ssend(Client, lindex, [Key, Index]).
lset(Client, Key, Index, Value) ->
    client:send(Client, lset, [[Key, Index, length(Value)],
                               [Value]]).
lrem(Client, Key, Number, Value) ->
    client:send(Client, lrem, [[Key, Number, length(Value)],
                               [Value]]).
lpop(Client, Key) -> client:ssend(Client, lpop, [Key]).
rpop(Client, Key) -> client:ssend(Client, rpop, [Key]).



%% Commands operating on sets
sadd(Client, Key, Value) -> internal_set_like(Client, sadd, Key, Value).
srem(Client, Key, Value) -> internal_set_like(Client, srem, Key, Value).
smove(Client, SrcKey, DstKey, Member) -> client:send(Client, smove, [[SrcKey, DstKey, length(Member)],
                                                                     [Member]]).
scard(Client, Key) -> client:ssend(Client, scard, [Key]).
sismember(Client, Key, Value) -> internal_set_like(Client, sismember, Key, Value).
sintersect(Client, Keys) -> client:ssend(Client, sinter, Keys).
sinter(Client, Keys) -> sintersect(Client, Keys).
sinterstore(Client, DstKey, Keys) -> client:ssend(Client, sinterstore, [DstKey|Keys]).
sunion(Client, Keys) -> client:ssend(Client, sunion, Keys).
sunionstore(Client, DstKey, Keys) -> client:ssend(Client, sunionstore, [DstKey|Keys]).
sdiff(Client, Keys) -> client:ssend(Client, sdiff, Keys).
sdiffstore(Client, DstKey, Keys) -> client:ssend(Client, sdiffstore, [DstKey|Keys]).
smembers(Client, Key) -> client:ssend(Client, smembers, [Key]).


%% Multiple DB commands
select(Client, Index) -> client:ssend(Client, select, [Index]).
move(Client, Key, DBIndex) -> client:ssend(Client, move, [Key, DBIndex]).
flushdb(Client) -> client:ssend(Client, flushdb).
flushall(Client) -> client:ssend(Client, flushall).


%% Commands operating on both lists and sets
sort(Client, Key) -> client:ssend(Client, sort, [Key]).
sort(Client, Key, Extra) -> client:ssend(Client, sort, [Key, Extra]).    


%% Persistence control commands
save(Client) -> client:ssend(Client, save).
bgsave(Client) -> client:ssend(Client, bgsave).
lastsave(Client) -> client:ssend(Client, lastsave).
shutdown(Client) -> client:asend(Client, shutdown).


%% Remote server control commands
info(Client) -> client:ssend(Client, info).
slaveof(Client, Host, Port) -> client:ssend(Client, slaveof, [Host, Port]).
slaveof(Client) -> client:ssend(Client, slaveof, ["no one"]).
