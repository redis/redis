-module(proto).

-export([parse/2]).

parse(empty, "+OK") ->
    ok;
parse(empty, "+PONG") ->
    pong;
parse(empty, ":0") ->
    false;
parse(empty, ":1") ->
    true;
parse(empty, "-" ++ Message) ->
    {error, Message};
parse(empty, "$-1") ->
    {read, nil};
parse(empty, "*-1") ->
    {hold, nil};
parse(empty, "$" ++ BulkSize) ->
    {read, list_to_integer(BulkSize)};
parse(read, "$" ++ BulkSize) ->
    {read, list_to_integer(BulkSize)};
parse(empty, "*" ++ MultiBulkSize) ->
    {hold, list_to_integer(MultiBulkSize)};
parse(empty, Message) ->
    convert(Message).

convert(":" ++ Message) ->
    list_to_integer(Message);
% in case the message is not OK or PONG it's a
% real value that we don't know how to convert
% to an atom, so just pass it as is and remove
% the +
convert("+" ++ Message) -> 
    Message;
convert(Message) ->
    Message.

