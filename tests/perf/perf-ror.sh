#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

shutdown_redis() {
    redis-cli -h $perf_host -p $perf_port credis_shutdown nosave 2>/dev/null
}

wait_redis_up() {
    while true; do
        ping_reply=$(redis-cli -h $perf_host -p $perf_port ping 2>/dev/null | tr -d '[:space:]')
        if [[ $ping_reply != PONG ]]; then echo -n .; sleep 1; else break; fi
    done
}

create_redis_conf() {
    local target_conf_file=$1
    cp -f $perf_base_conf $target_conf_file

	echo "#########  perf_configs ############" >> $target_conf_file
    for perf_config in ${perf_configs//,/ }
	do
		conf_key=$(echo $perf_config | awk -F= '{print $1}')
		conf_val=$(echo $perf_config | awk -F= '{print $2}')
		echo "$conf_key $conf_val" >> $target_conf_file
	done
}

start_redis() {
    $redis_server $perf_conf_file
}

create_rdb() {
    perf_rdb_file="$perf_job_dir/${perf_suite}.rdb"

    if [[ -f $perf_rdb_file ]]; then
        echo "create suite ok: $perf_rdb_file exitsts."
        return 0
    fi

	shutdown_redis
    sleep 1
	wait_redis_up
	echo "loading $perf_suite"

    if [[ $perf_suite == 10G_string ]]; then
		val_size=1k
		perf_write_cmd=set
		perf_read_cmd=get
        db_mb=10000
    elif [[ $perf_suite == 10G_hash ]]; then
		val_size=2k; # 5*400
		perf_write_cmd=hmset
		perf_read_cmd=hgetall
        db_mb=10000
    else
		echo "loaded failed: perf_suite $perf_suite not valid"
        return 1
    fi

	perf_key_count=$(($db_mb*1000/${val_size/k/}))
	echo "loading with: $perf_bench2 $perf_host:$perf_port $perf_password $perf_write_cmd $perf_key_count $key_prefix $load_nthd"
	$perf_bench2 $perf_host:$perf_port $perf_password $perf_write_cmd $perf_key_count $key_prefix $load_nthd" > $perf_suite-load-log

    redis-cli -h $host -p $port credis_save
    sleep 1
    redis-cli -h $host -p $port credis_shutdown nosave
    echo "loaded rdb $(du -sh $perf_rdb_file | awk '{print $1}') ok: $perf_rdb_file"
}

setup_redis() {
    shutdown_redis
    create_redis_conf $perf_conf_file
    start_redis

    current_memory=$(redis-cli -h $perf_host -p $perf_port info memory | grep 'used_memory:' | tr -d '[:space:]' | awk -F: '{print $2}')
    if [[ $perf_setup == cold ]]; then
        max_memory=$((current_memory+100*1024*1024))
        warmup_key_count=$(($key_count/3))
    elif [[ $perf_setup == warm ]]; then
        warmup_key_count=$(($key_count/3))
        max_memory=$((db_mb*1024*1024/5))
    elif [[ $perf_setup == hot ]]; then
        warmup_key_count=$(($key_count))
        max_memory=0
    else
        echo "setup failed"
        exit 1
    fi
    redis-cli -h $host -p $port config set maxmemory $max_memory
    echo "current_memory=$current_memory, max_memory=$max_memory"

    echo "warmup: $perf_bench2 $perf_host:$perf_port $perf_password $perf_read_cmd $warmup_key_count $key_prefix $load_nthd"
    $perf_bench2 $perf_host:$perf_port $perf_password $perf_read_cmd $warmup_key_count $key_prefix $load_nthd"

    echo "clean pagecache"
    sync; echo 1 > /proc/sys/vm/drop_caches

    echo "limit pagecache"
    redis_pid=$(redis-cli -h $host -p $port info server | grep process_id | tr -d '[:space:]' | awk -F: '{print $2}')
    cg_dir=/sys/fs/cgroup/memory/${perf_suite}-${perf_setup}
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
    echo $((400*1024*1024)) > $cg_limit_file

    echo "setup ok."
}

execute_perf_cases() {
    if ! [[ -d $perf_artifact_dir ]]; then mkdir -p $perf_artifact_dir; fi

    for perf_case in ${perf_cases//,/ }
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
            case_qps_limit=10000
        else
            echo "case invalid $perf_case"
        fi

        $bench2 $perf_host:$perf_port $perf_password $case_cmd $key_count $key_prefix $case_nthd $case_qps_limit > $perf_artifact_dir/$log_prefix-$perf_case-log
        echo "execute_perf_case $case_cmd-$case_load finished."
        sleep 10
	done
}

# check perf conditions & env
perf_sanity_check() {
    if [[ -z "$perf_base_dir" ]]; then perf_base_dir=/var/lib/k8s/test/ror_ci_perf ; fi
    if [[ -z "$perf_bench2" ]]; then perf_bench2="$perf_base_dir/bin/bench2"; fi
	if ! [[ -f "$perf_bench2" ]]; then echo "perf_bench2 not found: $perf_bench2"; return 1; fi
    if [[ -z "$perf_host" ]]; then echo "perf_host notset."; return 1; fi
    if [[ -z "$perf_port" ]]; then echo "perf_port notset."; return 1; fi
    if [[ -z "$redis_server" ]]; then redis_server=$SCRIPT_DIR/../../src/redis-server; return 1; fi
    if [[ -z "$perf_password" ]]; then perf_password=nopass; fi
    if [[ -z "$perf_job" ]]; then echo "perf_job notset."; return 1; fi
    if [[ -z "$perf_base_conf" ]] || ! [[ -f $perf_base_conf ]] ; then "perf_base_conf not valid: $perf_base_conf"; return 1; fi
    if [[ -z "$perf_suite"  ]]; then echo "perf_suite notset"; fi
    if [[ "$perf_suite" != "10G_string" && "$perf_suite" != 10G_hash ]]; then echo "perf_suite not valid"; return 1; fi
    if [[ -z "$perf_setup" ]]; then echo "perf_setup notset."; fi
    if [[ "$perf_setup" != cold && "$perf_setup" != warm && "$perf_setup" != hot ]]; then echo "perf_setup not valid"; return 1; fi
    if [[ -z "$perf_cases" ]]; then echo "perf_cases notset."; fi
	key_prefix=$perf_suite
    load_nthd=100
    perf_job_dir="$perf_base_dir/$perf_job"
    perf_artifact_dir="$perf_base_dir/$perf_job/artifacts"
    if ! [[ -d $perf_job_dir ]]; mkdir -p $perf_job_dir; fi
    perf_conf_file=$perf_job_dir/redis.conf
}

# set following env before executing perf:
# perf_job: different perf job should run parallel
# perf_suite: 10G_string/10G_hash
# perf_setup: cold_default/warm_default/hot_default
# perf_configs: cold_default/warm_default/hot_default
# perf_cases: set-100thd,get-1wqps...

if perf_sanity_check; then
    echo "perf aborted: sanity check failed."
    exit 1
fi

if create_perf_suite; then
    echo "perf aborted: create suite failed."
    exit 1
fi

if create_perf_setup; then
    echo "perf aborted: create suite failed."
    exit 1
fi

if execute_perf_cases; then
    echo "perf aborted: execute perf case failed."
    exit 1
fi

if collect_perf_reports; then
    echo "perf aborted: collect perf report failed."
    exit 1
fi

echo "perf succeeded."

