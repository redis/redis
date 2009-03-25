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
    gen_server:cast(Client, {send, sformat([Cmd|Args])}).

send(Client, Cmd) -> send(Client, Cmd, []).
send(Client, Cmd, Args) ->
    gen_server:cast(Client, {send,
        string:join([str(Cmd), format(Args)], " ")}).

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
            {ok, #redis{socket=Socket, calls=0}}
    end.

handle_call({send, Cmd}, From, State) ->
    gen_tcp:send(State#redis.socket, [Cmd|?EOL]),
    {noreply, State#redis{reply_caller=fun(V) -> gen_server:reply(From, lists:nth(1, V)) end,
                          remaining=1}};
        
handle_call(disconnect, _From, State) ->
    {stop, normal, ok, State};
handle_call(get_all_results, From, State) ->
    case State#redis.calls of
        0 ->
            % answers came earlier than we could start listening...
            % Very unlikely but totally possible.
            {reply, lists:reverse(State#redis.results), State#redis{results=[], calls=0}};
        _ ->
            % We are here earlier than results came, so just make
            % ourselves wait until stuff is ready.
            {noreply, State#redis{reply_caller=fun(V) -> gen_server:reply(From, V) end}}
    end;
handle_call(_, _From, State) -> {noreply, State}.


handle_cast({asend, Cmd}, State) ->
    gen_tcp:send(State#redis.socket, [Cmd|?EOL]),
    {noreply, State};
handle_cast({send, Cmd}, State=#redis{remaining=Remaining, calls=Calls}) ->
    % how we should do here: if remaining is already != 0 then we'll
    % let handle_info take care of keeping track how many remaining things
    % there are. If instead it's 0 we are the first call so let's just
    % do it.
    gen_tcp:send(State#redis.socket, [Cmd|?EOL]),
    case Remaining of
        0 ->
            {noreply, State#redis{remaining=1, calls=1}};
        _ ->
            {noreply, State#redis{calls=Calls+1}}
    end;
handle_cast(_Msg, State) -> {noreply, State}.


trim2({ok, S}) ->
    string:substr(S, 1, length(S)-2);
trim2(S) ->
    trim2({ok, S}).

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
save_or_reply(Result, State=#redis{calls=Calls, results=Results, reply_caller=ReplyCaller}) ->
    case Calls of
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
                           calls=0};
        _ ->
            State#redis{results=[Result|Results], remaining=1, pstate=empty, buffer=[], calls=Calls}

    end.

handle_info({tcp, Socket, Data}, State=#redis{calls=Calls}) ->
    Trimmed = trim2(Data),
    NewState = case {State#redis.remaining-1, proto:parse(State#redis.pstate, Trimmed)} of
        % This line contained an error code. Next line will hold
        % The error message that we will parse.
        {0, error} ->
            State#redis{remaining=1, pstate=error};

        % The stateful parser just started and tells us the number
        % of results that we will have to parse for those calls
        % where more than one result is expected. The next
        % line will start with the first item to read.
        {0, {hold, Remaining}} ->
            case Remaining of
                nil ->
                    save_or_reply(nil, State#redis{calls=Calls-1});
                _ ->
                    % Reset the remaining value to the number of results that we need to parse.
                    State#redis{remaining=Remaining, pstate=read}
            end;

        % We either had only one thing to read or we are at the
        % end of the stuff that we need to read. either way
        % just pack up the buffer and send.
        {0, {read, NBytes}} ->
            CurrentValue = case NBytes of
                nil ->
                    nil;
                _ ->
                    inet:setopts(Socket, [{packet, 0}]), % go into raw mode to read bytes
                    CV = trim2(gen_tcp:recv(Socket, NBytes+2)), % also consume the \r\n
                    inet:setopts(Socket, [{packet, line}]), % go back to line mode
                    CV
            end,
            OldBuffer = State#redis.buffer,
            case OldBuffer of
                [] ->
                    save_or_reply(CurrentValue, State#redis{calls=Calls-1});
                _ ->
                    save_or_reply(lists:reverse([CurrentValue|OldBuffer]), State#redis{calls=Calls-1})
            end;

        % The stateful parser tells us to read some bytes
        {N, {read, NBytes}} ->
            % annoying repetition... I should reuse this code.
            CurrentValue = case NBytes of
                nil ->
                    nil;
                _ ->
                    inet:setopts(Socket, [{packet, 0}]), % go into raw mode to read bytes
                    CV = trim2(gen_tcp:recv(Socket, NBytes+2)), % also consume the \r\n
                    inet:setopts(Socket, [{packet, line}]), % go back to line mode
                    CV
            end,
            OldBuffer = State#redis.buffer,
            State#redis{remaining=N, buffer=[CurrentValue|OldBuffer], pstate=read};


        % Simple return values contained in a single line
        {0, Value} ->
            save_or_reply(Value, State#redis{calls=Calls-1})

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


