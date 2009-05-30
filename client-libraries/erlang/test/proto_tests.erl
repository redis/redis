-module(proto_tests).

-include_lib("eunit/include/eunit.hrl").

parse_test() ->
    ok = proto:parse(empty, "+OK"),
    pong = proto:parse(empty, "+PONG"),
    false = proto:parse(empty, ":0"),
    true = proto:parse(empty, ":1"),
    {error, "1"} = proto:parse(empty, "-1").
