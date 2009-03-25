-module(erldis_tests).

-include_lib("eunit/include/eunit.hrl").
-include("erldis.hrl").

quit_test() ->
    {ok, Client} = erldis:connect("localhost"),
    ok = erldis:quit(Client),
    false = is_process_alive(Client).
    
utils_test() ->
    ?assertEqual(client:str(1), "1"),
    ?assertEqual(client:str(atom), "atom"),
    ?assertEqual(client:format([[1, 2, 3]]), "1 2 3"),
    ?assertEqual(client:format([[1,2,3], [4,5,6]]), "1 2 3\r\n4 5 6").

pipeline_test() ->
    {ok, Client} = erldis:connect("localhost"),
    erldis:flushall(Client),
    erldis:get(Client, "pippo"),
    erldis:set(Client, "hello", "kitty!"),
    erldis:setnx(Client, "foo", "bar"),
    erldis:setnx(Client, "foo", "bar"),
    [ok, nil, ok, true, false] = erldis:get_all_results(Client),

    erldis:exists(Client, "hello"),
    erldis:exists(Client, "foo"),
    erldis:get(Client, "foo"),
    erldis:mget(Client, ["hello", "foo"]),
    erldis:del(Client, "hello"),
    erldis:del(Client, "foo"),
    erldis:exists(Client, "hello"),
    erldis:exists(Client, "foo"),
    [true, true, "bar", ["kitty!", "bar"], true, true, false, false] = erldis:get_all_results(Client),
    
    erldis:set(Client, "pippo", "pluto"),
    erldis:sadd(Client, "pippo", "paperino"),
    % foo doesn't exist, the result will be nil
    erldis:lrange(Client, "foo", 1, 2),
    erldis:lrange(Client, "pippo", 1, 2),
    [ok,
     {error, "ERR Operation against a key holding the wrong kind of value"},
     nil,
     {error, "ERR Operation against a key holding the wrong kind of value"}
    ] = erldis:get_all_results(Client),
    erldis:del(Client, "pippo"),
    [true] = erldis:get_all_results(Client),

    erldis:rpush(Client, "a_list", "1"),
    erldis:rpush(Client, "a_list", "2"),
    erldis:rpush(Client, "a_list", "3"),
    erldis:rpush(Client, "a_list", "1"),
    erldis:lrem(Client, "a_list", 1, "1"),
    erldis:lrange(Client, "a_list", 0, 2),
    [ok, ok, ok, ok, true, ["2", "3", "1"]] = erldis:get_all_results(Client),

    erldis:sort(Client, "a_list"),
    erldis:sort(Client, "a_list", "DESC"), 
    erldis:lrange(Client, "a_list", 0, 2),
    erldis:sort(Client, "a_list", "LIMIT 0 2 ASC"),
    [["1", "2", "3"], ["3", "2", "1"], ["2", "3", "1"],
     ["1", "2"]] = erldis:get_all_results(Client),

    ok = erldis:quit(Client).



% inline_tests(Client) ->
%     [?_assertMatch(ok, erldis:set(Client, "hello", "kitty!")),
%      ?_assertMatch(false, erldis:setnx(Client, "hello", "kitty!")),
%      ?_assertMatch(true, erldis:exists(Client, "hello")),
%      ?_assertMatch(true, erldis:del(Client, "hello")),
%      ?_assertMatch(false, erldis:exists(Client, "hello")),
% 
%      ?_assertMatch(true, erldis:setnx(Client, "hello", "kitty!")),
%      ?_assertMatch(true, erldis:exists(Client, "hello")),
%      ?_assertMatch("kitty!", erldis:get(Client, "hello")),
%      ?_assertMatch(true, erldis:del(Client, "hello")),
%       
%       
%      ?_assertMatch(1, erldis:incr(Client, "pippo"))
%      ,?_assertMatch(2, erldis:incr(Client, "pippo"))
%      ,?_assertMatch(1, erldis:decr(Client, "pippo"))
%      ,?_assertMatch(0, erldis:decr(Client, "pippo"))
%      ,?_assertMatch(-1, erldis:decr(Client, "pippo"))
%      
%      ,?_assertMatch(6, erldis:incrby(Client, "pippo", 7))
%      ,?_assertMatch(2, erldis:decrby(Client, "pippo", 4))
%      ,?_assertMatch(-2, erldis:decrby(Client, "pippo", 4))
%      ,?_assertMatch(true, erldis:del(Client, "pippo"))
%     ].
