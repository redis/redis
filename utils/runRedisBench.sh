#!/bin/bash -e

################################################################################
# Global settings
################################################################################

USER=fedora

# Client-server communication interfaces
USE_UNIX_SOCKETS=0

# Redis default server port
SERVER_PORT=6379

SPINNER="-\|/"
SPINNER_POS=0

################################################################################
# Function definitions
################################################################################

get_timestamp() {
	date +"%Y%m%d%H%M%S"
}

print_help() {
	echo "Usage:"
	echo "runRedisBench.sh [options]"
	echo "where accepted options are:"
	echo "  -h|--help                      print this help message"
	echo "  -s=|--servers=N                start N redis servers on ports 6379, ..., 6379 + N - 1"
	echo "  -c=|--clients=N                start N redis clients per thread"
	echo "  -t=|--threads=N                start N threads with redis clients"
	echo "  -r=|--requests=N               send N requests per client"
	echo "  --sa=|--server-addr=ADDR       use ADDR as address of a host with redis servers"
	echo "  --ca=|--client-addr=ADDR       use ADDR as address of a host with redis clients (memtier_benchmark)"
	echo "  --sn=|--server-numa=N          run redis servers only on NUMA node N"
	echo "  --cn=|--client-numa=N          run memtier (redis clients) only on NUMA node N"
	echo "  --si=|--server-interface=ADDR  use ADDR as redis servers interface"
	echo "  --us|--use-unix-sockets        use unix sockets for client-server communication"
	echo "  --sc=|--server-config=FILE     use FILE as redis config file (the path is on the server host)"
	echo "  --wr=|--wr-ratio=RATIO         use RATIO for write:read ratio"
	echo "  --tag=WORD                     add WORD as a tag to output log file names"
	echo "Each use of --ca option adds a client node to the test."
	echo "If N client nodes are defined, also the options --si has to be used N times."
	echo "The --cn option also can be used several times (for each client node)."
}

getOptionsAddr() {
	local arg
	for arg in "$@"; do
		case $arg in
			-h|--help)
			print_help
			exit 0
			;;
			--sa=*|--server-addr=*)
			SERVER_ADDR="${arg#*=}"
			;;
			--ca=*|--client-addr=*)
			CLIENT_ADDR+=("${arg#*=}")
			;;
		esac
	done
}

getOptions() {
	CLIENT_ADDR=()
	local arg
	for arg in "$@"; do
		case $arg in
			-h|--help)
			print_help
			exit 0
			;;
			-s=*|--servers=*)
			SERVERS="${arg#*=}"
			;;
			-c=*|--clients=*)
			CLIENTS="${arg#*=}"
			;;
			-t=*|--threads=*)
			THREADS="${arg#*=}"
			;;
			-r=*|--requests=*)
			REQUESTS="${arg#*=}"
			;;
			-e=*|--records=*)
			RECORDS="${arg#*=}"
			;;
			--sa=*|--server-addr=*)
			SERVER_ADDR="${arg#*=}"
			;;
			--ca=*|--client-addr=*)
			CLIENT_ADDR+=("${arg#*=}")
			;;
			--si=*|--server-interface=*)
			SERVER_IFC+=("${arg#*=}")
			;;
			--sc=*|--server-config=*)
			SERVER_CONFIG="${arg#*=}"
			;;
			--sn=*|--server-numa=*)
			SERVER_NUMA="${arg#*=}"
			;;
			--cn=*|--client-numa=*)
			CLIENT_NUMA+=("${arg#*=}")
			;;
			--us|--use-unix-sockets)
			USE_UNIX_SOCKETS=1
			;;
			--tag=*)
			TAG="${arg#*=}"
			;;
			--wr=*|--wr-ratio=*)
			WR_RATIO="${arg#*=}"
			;;
			--user=*)
			USER="${arg#*=}"
			;;
		esac
	done
}

getKernelVersions() {
	SERVER_KERNEL=`ssh $USER@$SERVER_ADDR "uname -r"`
	for client in ${CLIENT_ADDR[@]}; do
		CLIENT_KERNEL+=(`ssh $USER@$client "uname -r"`)
	done
}

readServerConf() {
	# Not very secure ... be careful
        local vars=`ssh $USER@$SERVER_ADDR "cat /home/$USER/redisServer.conf"`
        eval $vars
	# while read -r line || [[ -n "$line" ]]; do
		## eval "$line"
		# export "$line"
	# done <<< `ssh $USER@$SERVER_ADDR "cat /home/$USER/redisServer.conf"`
}

readClientConf() {
	# Not very secure ... be careful
	for client in ${CLIENT_ADDR[@]}; do
		local vars=`ssh $USER@$client "cat /home/$USER/redisClient.conf"`
		eval $vars
	done
	# while read -r line || [[ -n "$line" ]]; do
		## eval "$line"
		# export "$line"
	# done <<< `ssh $USER@$CLIENT_ADDR "cat /home/$USER/redisClient.conf"`
}

readConfiguration() {
	readServerConf
	readClientConf
	getKernelVersions
}

checkAddr() {
	if [ -z "$SERVER_ADDR" ]; then echo SERVER_ADDR is not set; exit 1; fi
	if [ -z "${CLIENT_ADDR[0]}" ]; then echo CLIENT_ADDR is not set; exit 1; fi
}

print_spinner() {
	printf %b "$1 ${SPINNER:$SPINNER_POS:1}         \r"
	SPINNER_POS=$(((SPINNER_POS + 1) % ${#SPINNER}))
}

check() {
	if [ -z "$SERVERS" ]; then echo SERVERS is not set; exit 1; fi
	if [ -z "$CLIENTS" ]; then echo CLIENTS is not set; exit 1; fi
	if [ -z "$THREADS" ]; then echo THREADS is not set; exit 1; fi
	if [ -z "$REQUESTS" ]; then echo REQUESTS is not set; exit 1; fi
	if [ -z "$RECORDS" ]; then echo RECORDS is not set; exit 1; fi
	if [ -z "$SERVER_ADDR" ]; then echo SERVER_ADDR is not set; exit 1; fi
	if [ -z "$CLIENT_ADDR" ]; then echo CLIENT_ADDR is not set; exit 1; fi
	if [ -z "$SERVER_REDIS_DIR" ]; then echo SERVER_REDIS_DIR is not set; exit 1; fi
	if [ -z "$SERVER_MEMKIND_DIR" ]; then echo SERVER_MEMKIND_DIR is not set; exit 1; fi
	if [ -z "$SERVER_LOGS_DIR" ]; then echo SERVER_LOGS_DIR is not set; exit 1; fi
	if [ -z "$SERVER_CONFIG" ]; then echo SERVER_CONFIG is not set; exit 1; fi
	local i
	for (( i=0; i<${#CLIENT_ADDR[@]}; i++ )); do
		if [ -z "${CLIENT_MEMKIND_DIR[$i]}" ]; then echo CLIENT_MEMKIND_DIR is not set for client $i node ${CLIENT_ADDR[$i]}; exit 1; fi
		if [ -z "${CLIENT_LOGS_DIR[$i]}" ]; then echo CLIENT_LOGS_DIR is not set for client node ${CLIENT_ADDR[$i]}; exit 1; fi
		if [ -z "${SERVER_IFC[$i]}" -a "$USE_UNIX_SOCKETS" -eq "0" ]; then echo SERVER_IFC must be set or unix sockets must be used for client $i node ${CLIENT_ADDR[$i]}; exit 1; fi
	done
	if [ -z "$WR_RATIO" ]; then echo WR_RATIO is not set; exit 1; fi
}

printConfiguration() {
	echo "###################"
	echo "# Configuration:"
	echo "# No. of client nodes: ${#CLIENT_ADDR[@]}"
	echo "# No. of servers per client node: $SERVERS"
	echo "# No. of threads: $THREADS"
	echo "# No. of clients per thread: $CLIENTS"
	echo "# No. of requests per client: $REQUESTS"
	echo "# No. of records in each database: $RECORDS"
	echo "# Server host address: $SERVER_ADDR"
	echo "# Client host address(es): ${CLIENT_ADDR[@]}"
	echo "# Server host kernel: $SERVER_KERNEL"
	echo "# Client host kernel(s): ${CLIENT_KERNEL[@]}"
	echo "# Server interface(s): ${SERVER_IFC[@]}"
	echo "# Use unix sockets: $USE_UNIX_SOCKETS"
	echo "# Server NUMA node: $SERVER_NUMA"
	echo "# Client NUMA node(s): ${CLIENT_NUMA[@]}"
	echo "# Server redis dir: $SERVER_REDIS_DIR"
	echo "# Server memkind dir: $SERVER_MEMKIND_DIR"
	echo "# Client memkind dir(s): ${CLIENT_MEMKIND_DIR[@]}"
	echo "# Server logs dir: $SERVER_LOGS_DIR"
	echo "# Client logs dir(s): ${CLIENT_LOGS_DIR[@]}"
	echo "# Server config: $SERVER_CONFIG"
	echo "# Write/read ratio: $WR_RATIO"
	echo "# Log files tag: $TAG"
	echo "###################"
}

storeConfiguration() {
	if [ -n "$TAG" ]; then
		ssh $USER@$SERVER_ADDR "mkdir -p ${SERVER_LOGS_DIR}/$TAG"
	fi
	printConfiguration | ssh $USER@$SERVER_ADDR "cat > ${SERVER_LOGS_DIR}/${TAG+${TAG}/}configuration_${TAG}.txt"
	local i
	for (( i=0; i<${#CLIENT_ADDR[@]}; i++ )); do
		if [ -n "$TAG" ]; then
			ssh $USER@${CLIENT_ADDR[$i]} "mkdir -p ${CLIENT_LOGS_DIR[$i]}/$TAG"
		fi
		printConfiguration | ssh $USER@${CLIENT_ADDR[$i]} "cat > ${CLIENT_LOGS_DIR[$i]}/${TAG+${TAG}/}configuration_${TAG}.txt"
	done
}

startRedisServers() {
	if [ -z "$1" ]; then echo pass node number to start servers for; exit 1; fi
	local node=$1
	echo "Starting $SERVERS server(s) for client node $node (${CLIENT_ADDR[$node]})"
	local i
	for i in $(seq $SERVERS); do
		local port=$((SERVER_PORT + i - 1 + node * SERVERS))
		local out_file_base="redis_server_${TAG}_${SERVERS}_${i}_${node}"
		local cmd=""
		if [ -n "$SERVER_NUMA" ]; then
			cmd+="numactl -N $SERVER_NUMA"
		fi
		cmd+=" $SERVER_REDIS_DIR/src/redis-server $SERVER_CONFIG --appendonly no"
		if [ "$USE_UNIX_SOCKETS" == "1" ]; then 
			cmd+=" --unixsocket /tmp/redis$port.sock"
		else
			cmd+=" --port $port"
		fi
		# echo "start: $cmd"
		ssh -n -f $USER@$SERVER_ADDR "export LD_LIBRARY_PATH=${SERVER_MEMKIND_DIR}/.libs/; nohup $cmd > ${SERVER_LOGS_DIR}/${TAG+${TAG}/}${out_file_base}.log 2>&1 &"

	done
	echo "Done."
}

startAllRedisServers() {
	local i
	for (( i=0; i<${#CLIENT_ADDR[@]}; i++ )); do
		startRedisServers $i
	done
}

stopRedisServers() {
	echo "Stopping server(s)"
	ssh $USER@$SERVER_ADDR "pkill -f \"redis-server\"" || true
	echo "Done."
}

waitForProcessToFinish() {
	local IP_addr=$1
	local pattern=$2
	while [ $(ssh $USER@$IP_addr "pgrep -c -f \"$pattern\"") -ge 2 ]; do
		print_spinner
	       	sleep 1
	done
}

fillDatabases() {
	if [ -z "$1" ]; then echo pass node number to fill databases for; stopRedisServers; exit 1; fi
	local node=$1
	echo "Filling databases from client node $node (${CLIENT_ADDR[$node]})"
	local memtier_base_args="--ratio=1:0 -d 1024 -t $THREADS -c $CLIENTS --key-minimum=1 --key-maximum=$RECORDS -n $RECORDS"
	local i
	for i in $(seq $SERVERS); do
		local out_file_base="memtier_filldb_${TAG}_${SERVERS}_${i}_${node}"
		local memtier_args="$memtier_base_args --json-out-file=${CLIENT_LOGS_DIR[$node]}/${TAG+${TAG}/}${out_file_base}.json"
		local cmd=""
		if [ -n "${CLIENT_NUMA[$node]}" ]; then
			cmd+="numactl -N ${CLIENT_NUMA[$node]}"
		fi
		cmd+=" memtier_benchmark $memtier_args"
		local port=$((SERVER_PORT + i - 1 + node * SERVERS))
		if [ "$USE_UNIX_SOCKETS" == "1" ]; then 
			cmd+=" -S /tmp/redis$port.sock"
		else
			cmd+=" -s ${SERVER_IFC[$node]} -p $port"
		fi
		# echo "fill: $cmd"
		ssh -n -f $USER@${CLIENT_ADDR[$node]} "export LD_LIBRARY_PATH=${CLIENT_MEMKIND_DIR[$node]}/.libs/; nohup $cmd > ${CLIENT_LOGS_DIR[$node]}/${TAG+${TAG}/}${out_file_base}.log 2>&1 &"
	done
	waitForProcessToFinish ${CLIENT_ADDR[$node]} "memtier_benchmark"
	echo "Done."
}

fillAllDatabases() {
	local i
	for (( i=0; i<${#CLIENT_ADDR[@]}; i++ )); do
		fillDatabases $i
	done
}

runBenchmarks() {
	if [ -z "$1" ]; then echo pass node number to start benchmarks on; stopRedisServers; exit 1; fi
	local node=$1
	echo "Running benchmark from client node $node (${CLIENT_ADDR[$node]})"
	local memtier_base_args="--ratio=$WR_RATIO -d 1024 -t $THREADS -c $CLIENTS  --key-minimum=1 --key-maximum=$RECORDS -n $REQUESTS"
	local i
	for i in $(seq $SERVERS); do
		local out_file_base="memtier_bench_${TAG}_${SERVERS}_${i}_${node}"
		memtier_args="$memtier_base_args --json-out-file=${CLIENT_LOGS_DIR[$node]}/${TAG+${TAG}/}${out_file_base}.json"
		local cmd=""
		if [ -n "${CLIENT_NUMA[$node]}" ]; then
			cmd+="numactl -N ${CLIENT_NUMA[$node]}"
		fi
		cmd+=" memtier_benchmark $memtier_args"
		local port=$((SERVER_PORT + i - 1 + node * SERVERS))
		if [ "$USE_UNIX_SOCKETS" == "1" ]; then 
			cmd+=" -S /tmp/redis$port.sock"
		else
			cmd+=" -s ${SERVER_IFC[$node]} -p $port"
		fi
		# echo "bench: $cmd"
		ssh -n -f $USER@${CLIENT_ADDR[$node]} "export LD_LIBRARY_PATH=${CLIENT_MEMKIND_DIR[$node]}/.libs/; nohup $cmd > ${CLIENT_LOGS_DIR[$node]}/${TAG+${TAG}/}${out_file_base}.log 2>&1 &"
	done
	echo "Done."
}

runAllBenchmarks() {
	local i
	for (( i=0; i<${#CLIENT_ADDR[@]}; i++ )); do
		runBenchmarks $i
	done
}

getNoOfRunningBenchmarks() {
	local pattern="memtier_benchmark"
	local running=0
	local i
	for (( i=0; i<${#CLIENT_ADDR_UNIQUE[@]}; i++ )); do
		((running+=$(ssh $USER@${CLIENT_ADDR_UNIQUE[$i]} "pgrep -c -f \"$pattern\"")))
	done
	((running-=1))
	echo $running
}

getUniqueClients() {
	CLIENT_ADDR_UNIQUE=($(printf "%s\n" "${CLIENT_ADDR[@]}" | sort -u))
}

waitForBenchmarksToFinish() {
	echo "Waiting for benchmarks to finish"
	local running=$(getNoOfRunningBenchmarks)
	while [ "$running" -ge "1" ]; do
		print_spinner "running benchmarks: $running "
	       	sleep 1
		running=$(getNoOfRunningBenchmarks)
	done
	echo "Done.                                      "
}



################################################################################
# Your default test configuration
################################################################################

# SERVERS=2
# THREADS=3
# CLIENTS=5
# REQUESTS=100000
# RECORDS=180000
# SERVER_CONFIG=/home/fedora/git/redis/redis.conf

# Write:Read ratio
WR_RATIO="1:20"

################################################################################
# Test
################################################################################

getOptionsAddr $@
checkAddr
readConfiguration
getOptions $@
check
printConfiguration
storeConfiguration
getUniqueClients
startAllRedisServers
sleep 3
fillAllDatabases
sleep 3
runAllBenchmarks
waitForBenchmarksToFinish
sleep 3
# sleep 20
stopRedisServers

