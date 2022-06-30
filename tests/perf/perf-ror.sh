#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

shutdown_redis() {
    redis-cli -h $host -p $port credis_shutdown nosave 2>/dev/null
}

wait_redis_up() {
    while true; do
        ping_reply=$(redis-cli -h $host -p $port ping 2>/dev/null | tr -d '[:space:]')
        if [[ $ping_reply != PONG ]]; then echo -n .; sleep 1; else break; fi
    done
}

create_redis_conf() {
    local target_conf_file=$1
    cp -f $base_conf $target_conf_file
    echo "bind $host" >> $target_conf_file
    echo "port $port" >> $target_conf_file
    echo "dir $perf_runner_dir" >> $target_conf_file
	echo "#########  configs ############" >> $target_conf_file
    for config in ${configs//,/ }
	do
		conf_key=$(echo $config | awk -F= '{print $1}')
		conf_val=$(echo $config | awk -F= '{print $2}')
		echo "$conf_key $conf_val" >> $target_conf_file
	done
}

start_redis() {
    $redis_server $perf_conf_file
}

create_rdb() {
    perf_rdb_file="$perf_runner_dir/${suite}.rdb"

    if [[ -f $perf_rdb_file ]]; then
        echo "create rdb ok: $perf_rdb_file exitsts."
        return 0
    fi

	shutdown_redis
    sleep 1
	wait_redis_up
	echo "loading $suite"

    if [[ $suite == 10G_string ]]; then
		val_size=1k
		perf_write_cmd=set
		perf_read_cmd=get
        perf_db_mb=10000
    elif [[ $suite == 10G_hash ]]; then
		val_size=2k; # 5*400
		perf_write_cmd=hmset
		perf_read_cmd=hgetall
        perf_db_mb=10000
    else
		echo "loaded failed: suite $suite not valid"
        return 1
    fi

	perf_key_count=$(($perf_db_mb*1000/${val_size/k/}))
	echo "loading with: $bench2 $host:$port $password $perf_write_cmd $perf_key_count $key_prefix $load_nthd"
	$bench2 $host:$port $password $perf_write_cmd $perf_key_count $key_prefix $load_nthd" > $suite-load-log

    redis-cli -h $host -p $port credis_save
    sleep 1
    redis-cli -h $host -p $port credis_shutdown nosave
    echo "loaded rdb $(du -sh $perf_rdb_file | awk '{print $1}') ok: $perf_rdb_file"
}

setup_redis() {
    shutdown_redis
    create_redis_conf $perf_conf_file
    start_redis

    current_memory=$(redis-cli -h $host -p $port info memory | grep 'used_memory:' | tr -d '[:space:]' | awk -F: '{print $2}')
    if [[ $setup == cold ]]; then
        max_memory=$((current_memory+100*1024*1024))
        warmup_key_count=$(($key_count/3))
    elif [[ $setup == warm ]]; then
        warmup_key_count=$(($key_count/3))
        max_memory=$((perf_db_mb*1024*1024/5))
    elif [[ $setup == hot ]]; then
        warmup_key_count=$(($key_count))
        max_memory=0
    else
        echo "setup failed"
        exit 1
    fi
    redis-cli -h $host -p $port config set maxmemory $max_memory
    echo "current_memory=$current_memory, max_memory=$max_memory"

    echo "warmup: $bench2 $host:$port $password $perf_read_cmd $warmup_key_count $key_prefix $load_nthd"
    $bench2 $host:$port $password $perf_read_cmd $warmup_key_count $key_prefix $load_nthd"

    echo "clean pagecache"
    sync; echo 1 > /proc/sys/vm/drop_caches

    echo "limit pagecache"
    redis_pid=$(redis-cli -h $host -p $port info server | grep process_id | tr -d '[:space:]' | awk -F: '{print $2}')
    cg_dir=/sys/fs/cgroup/memory/${suite}-${setup}
    if [[ -d $cg_dir ]]; then 
        echo "cgroup mem dir($cg_dir) exists"
    else
        mkdir  $cg_dir
    fi

    cg_pid_file=$cg_dir/cgroup.procs
    cg_limit_file=$cg_dir/memory.limit_in_bytes
    existing_pids=$(cat $cg_pid_file)
    if [[ $existing_pids != "" ]]; then echo "cgroup pid($existing_pids) exists"; exit 1; fi

    echo $redis_pid > $cg_pid_file
    echo $((1024*1024*1024)) > $cg_limit_file

    echo "setup ok."
}

execute_cases() {
    if ! [[ -d $perf_artifact_dir ]]; then mkdir -p $perf_artifact_dir; fi

    for perf_case in ${cases//,/ }
	do
		case_cmd=$(echo $perf_case | awk -F= '{print $1}')
		case_load=$(echo $perf_case | awk -F= '{print $2}')
        if [[ $case_load == 100thd ]]; then
            case_nthd=100
            case_qps_limit=0
        elif [[ $case_load == 1wqps ]]; then
            case_nthd=1000
            case_qps_limit=10000
        elif [[ $case_load == 1kqps ]]; then
            case_nthd=100
            case_qps_limit=1000
        else
            echo "invalid case: $perf_case"
        fi

        $bench2 $host:$port $password $case_cmd $key_count $key_prefix $case_nthd $case_qps_limit > $perf_artifact_dir/$log_prefix-$perf_case-log
        echo "execute_perf_case $case_cmd-$case_load finished."
        sleep 10
	done
}

# check perf conditions & env
sanity_check() {
    if [[ -z "$base_dir" ]]; then base_dir=/var/lib/k8s/test/ror_ci_perf ; fi
    if [[ -z "$bench2" ]]; then bench2="$base_dir/bin/bench2"; fi
	if ! [[ -f "$bench2" ]]; then echo "bench2 not found: $bench2"; return 1; fi
    if [[ -z "$host" ]]; then echo "host notset."; return 1; fi
    if [[ -z "$port" ]]; then echo "port notset."; return 1; fi
    if [[ -z "$redis_server" ]]; then redis_server=$SCRIPT_DIR/../../src/redis-server; return 1; fi
    if [[ -z "$password" ]]; then password=nopass; fi
    if [[ -z "$runner" ]]; then echo "runner notset."; return 1; fi
    if [[ -z "$base_conf" ]] || ! [[ -f $base_conf ]] ; then "base_conf not valid: $base_conf"; return 1; fi
    if [[ -z "$suite"  ]]; then echo "suite notset"; fi
    if [[ "$suite" != "10G_string" && "$suite" != 10G_hash ]]; then echo "suite not valid"; return 1; fi
    if [[ -z "$setup" ]]; then echo "setup notset."; fi
    if [[ "$setup" != cold && "$setup" != warm && "$setup" != hot ]]; then echo "setup not valid"; return 1; fi
    if [[ -z "$cases" ]]; then echo "cases notset."; fi
	key_prefix=$suite
    load_nthd=100
    perf_runner_dir="$base_dir/$runner"
    perf_artifact_dir="$base_dir/$runner/artifacts"
    if ! [[ -d $perf_runner_dir ]]; mkdir -p $perf_runner_dir; fi
    perf_conf_file=$perf_runner_dir/redis.conf
}

collect_reports() {
    tar -czf $runner-$suite-$setup-$configs-$cases-$(date +%s).tgz $perf_artifact_dir
}

# set following env before executing perf:
#### required ####
# host: ror host
# port: ror port
# base_conf: base redis.conf
# runner: different perf runner should run parallel
# suite: 10G_string/10G_hash
# setup: cold_default/warm_default/hot_default
# configs: cold_default/warm_default/hot_default
# cases: set-100thd,get-1wqps...
#### optional ####
# base_dir: base directory to place rdb, artifacts, binary
# bench2: path to bench2 binary
# redis_server: path to redis-server binary
# password: redis-server password

if [[ "$EUID" != 0 ]]; then 
    echo "Please run as root"
    exit 1
fi

if sanity_check; then
    echo "perf aborted: sanity check failed."
    exit 1
fi

if create_rdb; then
    echo "perf aborted: create rdb failed."
    exit 1
fi

if setup_redis; then
    echo "perf aborted: setup redis failed."
    exit 1
fi

if execute_cases; then
    echo "perf aborted: execute cases failed."
    exit 1
fi

if collect_reports; then
    echo "perf aborted: collect report failed."
    exit 1
fi

echo "all succeeded."

