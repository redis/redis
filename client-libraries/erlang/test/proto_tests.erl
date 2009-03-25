-module(proto_tests).

-include_lib("eunit/include/eunit.hrl").

parse_test() ->
    ok = proto:parse(empty, "+OK"),
    pong = proto:parse(empty, "+PONG"),
    false = proto:parse(empty, ":0"),
    true = proto:parse(empty, ":1"),
    {error, no_such_key} = proto:parse(empty, "-1").
