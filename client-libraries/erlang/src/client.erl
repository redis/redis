-module(client).
-behavior(gen_server).

-export([start/1, start/2, connect/1, connect/2, asend/2, send/3, send/2,
         disconnect/1, ssend/3, str/1, format/1, sformat/1, ssend/2,
         get_all_results/1]).
-export([init/1, handle_call/3, handle_cast/2,
         handle_info/2, terminate/2, code_change/3]).

-include("erldis.hrl").

-define(EOL, "\r\n").


%% Helpers
str(X) when is_list(X) ->
    X;
str(X) when is_atom(X) ->
    atom_to_list(X);
str(X) when is_binary(X) ->
    binary_to_list(X);
str(X) when is_integer(X) ->
    integer_to_list(X);
str(X) when is_float(X) ->
    float_to_list(X).

format([], Result) ->
    string:join(lists:reverse(Result), ?EOL);
format([Line|Rest], Result) ->
    JoinedLine = string:join([str(X) || X <- Line], " "),
    format(Rest, [JoinedLine|Result]).

format(Lines) ->
    format(Lines, []).
sformat(Line) ->
    format([Line], []).

get_parser(Cmd)
    when Cmd =:= set orelse Cmd =:= setnx orelse Cmd =:= del
        orelse Cmd =:= exists orelse Cmd =:= rename orelse Cmd =:= renamenx
        orelse Cmd =:= rpush orelse Cmd =:= lpush orelse Cmd =:= ltrim
        orelse Cmd =:= lset orelse Cmd =:= sadd orelse Cmd =:= srem
        orelse Cmd =:= sismember orelse Cmd =:= select orelse Cmd =:= move
        orelse Cmd =:= save orelse Cmd =:= bgsave orelse Cmd =:= flushdb
        orelse Cmd =:= flushall ->
    fun proto:parse/2;
get_parser(Cmd) when Cmd =:= lrem ->
    fun proto:parse_special/2;
get_parser(Cmd)
    when Cmd =:= incr orelse Cmd =:= incrby orelse Cmd =:= decr
        orelse Cmd =:= decrby orelse Cmd =:= llen orelse Cmd =:= scard ->
    fun proto:parse_int/2;
get_parser(Cmd) when Cmd =:= type ->
    fun proto:parse_types/2;
get_parser(Cmd) when Cmd =:= randomkey ->
    fun proto:parse_string/2;
get_parser(Cmd)
    when Cmd =:= get orelse Cmd =:= lindex orelse Cmd =:= lpop
        orelse Cmd =:= rpop ->
    fun proto:single_stateful_parser/2;
get_parser(Cmd)
    when Cmd =:= keys orelse Cmd =:= lrange orelse Cmd =:= sinter
        orelse Cmd =:= smembers orelse Cmd =:= sort ->
    fun proto:stateful_parser/2.
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


%% Exported API
start(Host) ->
    connect(Host).
start(Host, Port) ->
    connect(Host, Port).

connect(Host) ->
    connect(Host, 6379).
connect(Host, Port) ->
    gen_server:start_link(?MODULE, [Host, Port], []).

ssend(Client, Cmd) -> ssend(Client, Cmd, []).
ssend(Client, Cmd, Args) ->
    gen_server:cast(Client, {send, sformat([Cmd|Args]), get_parser(Cmd)}).

send(Client, Cmd) -> send(Client, Cmd, []).
send(Client, Cmd, Args) ->
    gen_server:cast(Client, {send,
        string:join([str(Cmd), format(Args)], " "), get_parser(Cmd)}).

asend(Client, Cmd) ->
    gen_server:cast(Client, {asend, Cmd}).
disconnect(Client) ->
    gen_server:call(Client, disconnect).

get_all_results(Client) ->
    gen_server:call(Client, get_all_results).
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%



%% gen_server callbacks
init([Host, Port]) ->
    process_flag(trap_exit, true),
    ConnectOptions = [list, {active, once}, {packet, line}, {nodelay, true}],
    case gen_tcp:connect(Host, Port, ConnectOptions) of
        {error, Why} ->
            {error, {socket_error, Why}};
        {ok, Socket} ->
            {ok, #redis{socket=Socket, parsers=queue:new()}}
    end.

handle_call({send, Cmd, Parser}, From, State=#redis{parsers=Parsers}) ->
    gen_tcp:send(State#redis.socket, [Cmd|?EOL]),
    {noreply, State#redis{reply_caller=fun(V) -> gen_server:reply(From, lists:nth(1, V)) end,
                          parsers=queue:in(Parser, Parsers), remaining=1}};
        
handle_call(disconnect, _From, State) ->
    {stop, normal, ok, State};
handle_call(get_all_results, From, State) ->
    case queue:is_empty(State#redis.parsers) of
        true ->
            % answers came earlier than we could start listening...
            % Very unlikely but totally possible.
            {reply, lists:reverse(State#redis.results), State#redis{results=[]}};
        false ->
            % We are here earlier than results came, so just make
            % ourselves wait until stuff is ready.
            {noreply, State#redis{reply_caller=fun(V) -> gen_server:reply(From, V) end}}
    end;
handle_call(_, _From, State) -> {noreply, State}.


handle_cast({asend, Cmd}, State) ->
    gen_tcp:send(State#redis.socket, [Cmd|?EOL]),
    {noreply, State};
handle_cast({send, Cmd, Parser}, State=#redis{parsers=Parsers, remaining=Remaining}) ->
    % how we should do here: if remaining is already != 0 then we'll
    % let handle_info take care of keeping track how many remaining things
    % there are. If instead it's 0 we are the first call so let's just
    % do it.
    gen_tcp:send(State#redis.socket, [Cmd|?EOL]),
    NewParsers = queue:in(Parser, Parsers),
    case Remaining of
        0 ->
            {noreply, State#redis{remaining=1, parsers=NewParsers}};
        _ ->
            {noreply, State#redis{parsers=NewParsers}}
    end;
handle_cast(_Msg, State) -> {noreply, State}.


trim2({ok, S}) ->
    string:substr(S, 1, length(S)-2);
trim2(S) ->
    trim2({ok, S}).

% This is useful to know if there are more messages still coming.
get_remaining(ParsersQueue) ->
    case queue:is_empty(ParsersQueue) of
        true -> 0;
        false -> 1
    end.

% This function helps with pipelining by creating a pubsub system with
% the caller. The caller could submit multiple requests and not listen
% until later when all or some of them have been answered, at that
% point 2 conditions can be true:
%  1) We still need to process more things in this response chain
%  2) We are finished.
%
% And these 2 are together with the following 2:
%  1) We called get_all_results before the end of the responses.
%  2) We called get_all_results after the end of the responses.
%
% If there's stuff missing in the chain we just push results, this also
% happens when there's nothing more to process BUT we haven't requested
% results yet.
% In case we have requested results: if requests are not yet ready we
% just push them, otherwise we finally answer all of them.
save_or_reply(Result, State=#redis{results=Results, reply_caller=ReplyCaller, parsers=Parsers}) ->
    case get_remaining(Parsers) of
        1 ->
            State#redis{results=[Result|Results], remaining=1, pstate=empty, buffer=[]};
        0 ->
            % We don't reverse results here because if all the requests
            % come in and then we submit another one, if we reverse
            % they will be scrambled in the results field of the record.
            % instead if we wait just before we reply they will be
            % in the right order.
            FullResults = [Result|Results],
            NewState = case ReplyCaller of
                undefined ->
                    State#redis{results=FullResults};
                _ ->
                    ReplyCaller(lists:reverse(FullResults)),
                    State#redis{results=[]}
            end,
            NewState#redis{remaining=0, pstate=empty,
                           reply_caller=undefined, buffer=[],
                           parsers=Parsers}
    end.

handle_info({tcp, Socket, Data}, State) ->
    {{value, Parser}, NewParsers} = queue:out(State#redis.parsers),
    Trimmed = trim2(Data),
    NewState = case {State#redis.remaining-1, Parser(State#redis.pstate, Trimmed)} of
        % This line contained an error code. Next line will hold
        % The error message that we will parse.
        {0, error} ->
            % reinsert the parser in the front, next step is still gonna be needed
            State#redis{remaining=1, pstate=error,
                    parsers=queue:in_r(Parser, NewParsers)};

        % The stateful parser just started and tells us the number
        % of results that we will have to parse for those calls
        % where more than one result is expected. The next
        % line will start with the first item to read.
        {0, {hold, Remaining}} ->
            % Reset the remaining value to the number of results
            % that we need to parse.
            % and reinsert the parser in the front, next step is still gonna be needed
            State#redis{remaining=Remaining, pstate=read,
                    parsers=queue:in_r(Parser, NewParsers)};

        % We either had only one thing to read or we are at the
        % end of the stuff that we need to read. either way
        % just pack up the buffer and send.
        {0, {read, NBytes}} ->
            inet:setopts(Socket, [{packet, 0}]), % go into raw mode to read bytes
            CurrentValue = trim2(gen_tcp:recv(Socket, NBytes+2)), % also consume the \r\n
            inet:setopts(Socket, [{packet, line}]), % go back to line mode
            OldBuffer = State#redis.buffer,
            case OldBuffer of
                [] ->
                    save_or_reply(CurrentValue, State#redis{parsers=NewParsers});
                _ ->
                    save_or_reply(lists:reverse([CurrentValue|OldBuffer]), State#redis{parsers=NewParsers})
            end;


        % The stateful parser tells us to read some bytes
        {N, {read, NBytes}} ->
            inet:setopts(Socket, [{packet, 0}]), % go into raw mode to read bytes
            CurrentValue = trim2(gen_tcp:recv(Socket, NBytes+2)), % also consume the \r\n
            inet:setopts(Socket, [{packet, line}]), % go back to line mode
            OldBuffer = State#redis.buffer,
            State#redis{remaining=N, buffer=[CurrentValue|OldBuffer],
                pstate=read, parsers=queue:in_r(Parser, NewParsers)};


        % Simple return values contained in a single line
        {0, Value} ->
            save_or_reply(Value, State#redis{parsers=NewParsers})

    end,
    inet:setopts(Socket, [{active, once}]),
    {noreply, NewState};
handle_info(_Info, State) -> {noreply, State}.


terminate(_Reason, State) ->
    case State#redis.socket of
        undefined ->
            pass;
        Socket ->
            gen_tcp:close(Socket)
    end,
    ok.


code_change(_OldVsn, State, _Extra) -> {ok, State}.
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%


