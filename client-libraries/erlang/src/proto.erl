-module(proto).

-export([parse/2, parse_int/2, parse_types/2,
	 parse_string/2, stateful_parser/2,
	 single_stateful_parser/2, parse_special/2]).


parse(empty, "+OK") ->
    ok;
parse(empty, "+PONG") ->
    pong;
parse(empty, "0") ->
    false;
parse(empty, "1") ->
    true;
parse(empty, "-1") ->
    {error, no_such_key};
parse(empty, "-2") ->
    {error, wrong_type};
parse(empty, "-3") ->
    {error, same_db};
parse(empty, "-4") ->
    {error, argument_out_of_range};
parse(empty, "-" ++ Message) ->
    {error, Message}.

parse_special(empty, "-1") ->
    parse(empty, "-1");
parse_special(empty, "-2") ->
    parse(empty, "-2");
parse_special(empty, N) ->
    list_to_integer(N).

parse_int(empty, "-ERR " ++ Message) ->
    {error, Message};
parse_int(empty, Value) ->
    list_to_integer(Value).

parse_string(empty, Message) ->
    Message.

parse_types(empty, "none") -> none;
parse_types(empty, "string") -> string;
parse_types(empty, "list") -> list;
parse_types(empty, "set") -> set.


% I'm used when redis returns multiple results
stateful_parser(empty, "nil") ->
    nil;
stateful_parser(error, "-ERR " ++ Error) ->
    {error, Error};
stateful_parser(empty, "-" ++ _ErrorLength) ->
    error;
stateful_parser(empty, NumberOfElements) ->
    {hold, list_to_integer(NumberOfElements)};
stateful_parser(read, ElementSize) ->
    {read, list_to_integer(ElementSize)}.

% I'm used when redis returns just one result
single_stateful_parser(empty, "nil") ->
    nil;
single_stateful_parser(error, "-ERR " ++ Error) ->
    {error, Error};
single_stateful_parser(empty, "-" ++ _ErrorLength) ->
    error;
single_stateful_parser(empty, ElementSize) ->
    {read, list_to_integer(ElementSize)}.
