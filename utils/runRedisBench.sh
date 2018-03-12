#!/bin/bash -e

################################################################################
# Global settings
################################################################################

USER=fedora
SERVER_HOME_DIR=/home/$USER
CLIENT_HOME_DIR=/home/$USER

# Client-server communication interfaces
USE_UNIX_SOCKETS=0

# Redis server port
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
	echo "  -h|--help                         print this help message"
	echo "  -s=N|--servers=N                  start N redis servers on ports 6379, ..., 6379 + N - 1"
	echo "  -c=N|--clients=N                  start N redis clients per thread"
	echo "  -t=N|--threads=N                  start N threads with redis clients"
	echo "  -r=N|--requests=N                 send N requests per client"
	echo "  --sa=ADDR|--server-addr=ADDR      use ADDR as address of a host with redis servers"
	echo "  --ca=ADDR|--client-addr=ADDR      use ADDR as address of a host with redis clients (memtier_benchmark)"
	echo "  --sn=N|--server-numa=N            run redis servers only on NUMA node N"
	echo "  --cn=N|--client-numa=N            run memtier (redis clients) only on NUMA node N"
	echo "  --si=ADDR|--server-interface=ADDR use ADDR as redis servers interface"
	echo "  --us|--use-unix-sockets           use unix sockets for client-server communication"
	echo "  --sc=FILE|--server-config=FILE    use FILE as redis config file"
	echo "  --tag=WORD                        add WORD as a tag to output log file names"
	echo "  --ts|--timestamp                  add timestamp as a prefix to output log file names"
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
			CLIENT_ADDR="${arg#*=}"
			;;
		esac
	done
}

getOptions() {
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
			--sa=*|--server-addr=*)
			SERVER_ADDR="${arg#*=}"
			;;
			--ca=*|--client-addr=*)
			CLIENT_ADDR="${arg#*=}"
			;;
			--si=*|--server-interface=*)
			SERVER_IFC="${arg#*=}"
			;;
			--sc=*|--server-config=*)
			SERVER_CONFIG="${arg#*=}"
			;;
			--sn=*|--server-numa=*)
			SERVER_NUMA="${arg#*=}"
			;;
			--cn=*|--client-numa=*)
			CLIENT_NUMA="${arg#*=}"
			;;
			--us|--use-unix-sockets)
			USE_UNIX_SOCKETS=1
			;;
			--tag=*)
			TAG="${arg#*=}_"
			;;
			--wr=*|--wr-ratio=*)
			WR_RATIO="${arg#*=}"
			;;
			--ts|--timestamp)
			TIMESTAMP="$(get_timestamp)_"
			;;
		esac
	done
}

getKernelVersions() {
	SERVER_KERNEL=`ssh $USER@$SERVER_ADDR "uname -r"`
	CLIENT_KERNEL=`ssh $USER@$CLIENT_ADDR "uname -r"`
}

readServerConf() {
        local vars=`ssh $USER@$SERVER_ADDR "cat $SERVER_HOME_DIR/redisServer.conf"`
        export $vars
	# while read -r line || [[ -n "$line" ]]; do
		## eval "$line"
		# export "$line"
	# done <<< `ssh $USER@$SERVER_ADDR "cat $SERVER_HOME_DIR/redisServer.conf"`
}

readClientConf() {
        local vars=`ssh $USER@$CLIENT_ADDR "cat $CLIENT_HOME_DIR/redisClient.conf"`
        export $vars
	# while read -r line || [[ -n "$line" ]]; do
		## eval "$line"
		# export "$line"
	# done <<< `ssh $USER@$CLIENT_ADDR "cat $CLIENT_HOME_DIR/redisClient.conf"`
}

readConfiguration() {
	readServerConf
	readClientConf
	getKernelVersions
}

checkAddr() {
	if [ -z "$SERVER_ADDR" ]; then echo SERVER_ADDR is not set; exit 1; fi
	if [ -z "$CLIENT_ADDR" ]; then echo CLIENT_ADDR is not set; exit 1; fi
}

print_spinner() {
	printf %b "${SPINNER:$SPINNER_POS:1} \r"
	SPINNER_POS=$(((SPINNER_POS + 1) % ${#SPINNER}))
}

check() {
	if [ -z "$SERVERS" ]; then echo SERVERS is not set; exit 1; fi
	if [ -z "$CLIENTS" ]; then echo CLIENTS is not set; exit 1; fi
	if [ -z "$THREADS" ]; then echo THREADS is not set; exit 1; fi
	if [ -z "$REQUESTS" ]; then echo REQUESTS is not set; exit 1; fi
	if [ -z "$SERVER_ADDR" ]; then echo SERVER_ADDR is not set; exit 1; fi
	if [ -z "$CLIENT_ADDR" ]; then echo CLIENT_ADDR is not set; exit 1; fi
	if [ -z "$SERVER_NUMA" ]; then echo SERVER_NUMA is not set; exit 1; fi
	if [ -z "$CLIENT_NUMA" ]; then echo CLIENT_NUMA is not set; exit 1; fi
	if [ -z "$SERVER_REDIS_DIR" ]; then echo SERVER_REDIS_DIR is not set; exit 1; fi
	if [ -z "$CLIENT_REDIS_DIR" ]; then echo CLIENT_REDIS_DIR is not set; exit 1; fi
	if [ -z "$SERVER_MEMKIND_DIR" ]; then echo SERVER_MEMKIND_DIR is not set; exit 1; fi
	if [ -z "$CLIENT_MEMKIND_DIR" ]; then echo CLIENT_MEMKIND_DIR is not set; exit 1; fi
	if [ -z "$SERVER_LOGS_DIR" ]; then echo SERVER_LOGS_DIR is not set; exit 1; fi
	if [ -z "$CLIENT_LOGS_DIR" ]; then echo CLIENT_LOGS_DIR is not set; exit 1; fi
	if [ -z "$SERVER_CONFIG" ]; then echo SERVER_CONFIG is not set; exit 1; fi
	if [ -z "$WR_RATIO" ]; then echo WR_RATIO is not set; exit 1; fi
	echo "server ifc $SERVER_IFC"
	echo "use sockets $USE_UNIX_SOCKETS"
	if [ -z "$SERVER_IFC" -a "$USE_UNIX_SOCKETS" -eq "0" ]; then echo SERVER_IFC must be set or unix sockets must be used; exit 1; fi
}

printConfiguration() {
	echo "###################"
	echo "# Configuration:"
	echo "# No. of servers: $SERVERS"
	echo "# No. of clients per thread: $CLIENTS"
	echo "# No. of threads: $THREADS"
	echo "# No. of requests per client: $REQUESTS"
	echo "# Server host address: $SERVER_ADDR"
	echo "# Client host address: $CLIENT_ADDR"
	echo "# Server host kernel: $SERVER_KERNEL"
	echo "# Client host kernel: $CLIENT_KERNEL"
	echo "# Server interface: $SERVER_IFC"
	echo "# Use unix sockets: $USE_UNIX_SOCKETS"
	echo "# Server NUMA node: $SERVER_NUMA"
	echo "# Client NUMA node: $CLIENT_NUMA"
	echo "# Server redis dir: $SERVER_REDIS_DIR"
	echo "# Client redis dir: $CLIENT_REDIS_DIR"
	echo "# Server memkind dir: $SERVER_MEMKIND_DIR"
	echo "# Client memkind dir: $CLIENT_MEMKIND_DIR"
	echo "# Server logs dir: $SERVER_LOGS_DIR"
	echo "# Client logs dir: $CLIENT_LOGS_DIR"
	echo "# Server config: $SERVER_CONFIG"
	echo "# Write/read ratio: $WR_RATIO"
	echo "# Timestamp: $TIMESTAMP"
	echo "# Log files tag: $TAG"
	echo "###################"
}

storeConfiguration() {
	printConfiguration | ssh $USER@$SERVER_ADDR "cat > ${SERVER_LOGS_DIR}/configuration_${TAG}.txt"
	printConfiguration | ssh $USER@$CLIENT_ADDR "cat > ${CLIENT_LOGS_DIR}/configuration_${TAG}.txt"
}

startRedisServers() {
	echo "Starting $SERVERS server(s)"
	for i in $(seq $SERVERS); do
		local port=$((SERVER_PORT + i - 1))
		local out_file_base="${TIMESTAMP}redis_server_${TAG}${SERVERS}_${i}"
		local cmd=""
		if [ -n "$SERVER_NUMA" ]; then
			cmd+="numactl -N $SERVER_NUMA"
		fi
		cmd+=" $SERVER_REDIS_DIR/src/redis-server $SERVER_CONFIG --appendonly no"
		if [ "$USE_UNIX_SOCKETS" == "1" ]; then 
			cmd+=" --unixsocket /tmp/redis$i.sock"
		else
			cmd+=" --port $port"
		fi
		# echo "start: $cmd"
		ssh -n -f $USER@$SERVER_ADDR "export LD_LIBRARY_PATH=${SERVER_MEMKIND_DIR}/.libs/; nohup $cmd > ${SERVER_LOGS_DIR}/${out_file_base}.log 2>&1 &"

	done
	echo "Done."
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
	echo "Filling databases"
	local memtier_base_args="--ratio=1:0 -d 1024 -t $THREADS -c $CLIENTS --key-minimum=1 --key-maximum=$REQUESTS -n $REQUESTS"
	for i in $(seq $SERVERS); do
		local out_file_base="${TIMESTAMP}memtier_filldb_${TAG}${SERVERS}_${i}"
		local memtier_args="$memtier_base_args --json-out-file=${CLIENT_LOGS_DIR}/${out_file_base}.json"
		local cmd=""
		if [ -n "$CLIENT_NUMA" ]; then
			cmd+="numactl -N $CLIENT_NUMA"
		fi
		cmd+=" memtier_benchmark $memtier_args" 
		local port=$((SERVER_PORT + i - 1))
		if [ "$USE_UNIX_SOCKETS" == "1" ]; then 
			cmd+=" -S /tmp/redis$i.sock"
		else
			cmd+=" -s $SERVER_IFC -p $port"
		fi
		# echo "fill: $cmd"
		ssh -n -f $USER@$CLIENT_ADDR "export LD_LIBRARY_PATH=${CLIENT_MEMKIND_DIR}/.libs/; nohup $cmd > ${CLIENT_LOGS_DIR}/${out_file_base}.log 2>&1 &"
	done
	waitForProcessToFinish $CLIENT_ADDR "memtier_benchmark"
	echo "Done."
}

runBenchmark() {
	echo "Running benchmark"
	local memtier_base_args="--ratio=$WR_RATIO -d 1024 -t $THREADS -c $CLIENTS  --key-minimum=1 --key-maximum=$REQUESTS -n $REQUESTS"
	for i in $(seq $SERVERS); do
		local out_file_base="${TIMESTAMP}memtier_bench_${TAG}${SERVERS}_${i}"
		memtier_args="$memtier_base_args --json-out-file=${CLIENT_LOGS_DIR}/${out_file_base}.json"
		local cmd=""
		if [ -n "$CLIENT_NUMA" ]; then
			cmd+="numactl -N $CLIENT_NUMA"
		fi
		cmd+=" memtier_benchmark $memtier_args"
		local port=$((SERVER_PORT + i - 1))
		if [ "$USE_UNIX_SOCKETS" == "1" ]; then 
			cmd+=" -S /tmp/redis$i.sock"
		else
			cmd+=" -s $SERVER_IFC -p $port"
		fi
		# echo "bench: $cmd"
		ssh -n -f $USER@$CLIENT_ADDR "export LD_LIBRARY_PATH=${CLIENT_MEMKIND_DIR}/.libs/; nohup $cmd > ${CLIENT_LOGS_DIR}/${out_file_base}.log 2>&1 &"
	done
	waitForProcessToFinish $CLIENT_ADDR "memtier_benchmark"
	echo "Done."
}

################################################################################
# Your default test configuration
################################################################################

# SERVERS=2
# THREADS=3
# CLIENTS=5
# REQUESTS=100000
# SERVER_CONFIG=/home/fedora/git/redis/redis.conf
SERVER_NUMA=0
CLIENT_NUMA=0

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
startRedisServers
sleep 3
fillDatabases
sleep 3
runBenchmark
sleep 3
# sleep 60
stopRedisServers

