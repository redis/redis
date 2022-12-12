#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

wait_redis() {
    local target_status=$1
    if [[ $target_status != "up" ]] && [[ $target_status != "down" ]] ; then
        echo "wait_redis: target status($1) invalid"
        return 1
    fi

    echo "waiting redis $host:$port $target_status."

    while true; do
        ping_reply=$(redis-cli -h $host -p $port ping 2>/dev/null | tr -d '[:space:]')

        if [[ ($target_status == up && $ping_reply != PONG) || ($target_status == down && $ping_reply == PONG) ]];  then
            echo -n .
            sleep 1
        else
            break
        fi
    done
    return 0
}

get_redis_pid() {
    local host=$1
    local port=$2
    local redis_pid=$(redis-cli -h $host -p $port info server | grep process_id | tr -d '[:space:]' | awk -F: '{print $2}')
    echo $redis_pid
}

shutdown_redis() {
    echo "shutting down redis $host:$port, pid=$(get_redis_pid $host $port)"
    redis-cli -h $host -p $port credis_shutdown nosave 2>/dev/null
	wait_redis down
}

start_redis() {
    echo "start redis $host:$port"
    $redis_server $conf_file
	wait_redis up
    echo "start redis $host:$port ok, pid=$(get_redis_pid $host $port)"
}

create_conf() {
    echo "creating $conf_file with base on: $base_conf"

    cp -f $base_conf $conf_file

	echo "#########  configs ##########" >> $conf_file
    echo "bind $host" >> $conf_file
    echo "port $port" >> $conf_file
    echo "dir $runner_dir" >> $conf_file
    echo "dbfilename ${suite}.rdb" >> $conf_file

    for config in ${configs//,/ }
	do
		conf_key=$(echo $config | awk -F= '{print $1}')
		conf_val=$(echo $config | awk -F= '{print $2}')
		echo "$conf_key $conf_val" >> $conf_file
	done
}

create_rdb() {
    echo "creating rdb: $rdb_file"

    if [[ -f $rdb_file ]]; then
        echo "create rdb ok: $rdb_file exitsts."
        return 0
    fi

	shutdown_redis
    start_redis

	echo "creating $suite"

	echo "init data with: $bench2 $host:$port $password $rdb_write_cmd $rdb_key_count $key_prefix $load_nthd"

    init_data_report_file=$artifact_dir/$log_prefix.init_data.log

	$bench2 $host:$port $password $rdb_write_cmd $rdb_key_count $key_prefix $load_nthd > $init_data_report_file

    redis-cli -h $host -p $port credis_save
    sleep 1
    redis-cli -h $host -p $port credis_shutdown nosave
    echo "create rdb $(du -sh $rdb_file | awk '{print $1}') ok: $rdb_file"

    return 0
}

setup_redis() {
    shutdown_redis
    start_redis

    current_memory=$(redis-cli -h $host -p $port info memory | grep 'used_memory:' | tr -d '[:space:]' | awk -F: '{print $2}')
    if [[ $setup == cold ]]; then
        warmup_key_count=$(($rdb_key_count/3))
        max_memory=$((current_memory+100*1024*1024))
    elif [[ $setup == warm ]]; then
        warmup_key_count=$(($rdb_key_count/3))
        max_memory=$((db_mb*1024*1024/4))
    elif [[ $setup == hot ]]; then
        warmup_key_count=$(($rdb_key_count))
        max_memory=0
    else
        echo "setup failed"
        exit 1
    fi

    redis-cli -h $host -p $port config set maxmemory $max_memory
    echo "setup redis $host:$port with $setup setup (current_memory=$((current_memory/1024/1024))MB, max_memory=$((max_memory/1024/1024))MB)"

    echo "warming up: $bench2 $host:$port $password $rdb_read_cmd $warmup_key_count $key_prefix $load_nthd"

    warmup_report_file=$artifact_dir/$log_prefix.warmup.log

    $bench2 $host:$port $password $rdb_read_cmd $warmup_key_count $key_prefix $load_nthd > $warmup_report_file

    echo "clean pagecache"
    sync; echo 1 > /proc/sys/vm/drop_caches

    echo "limiting pagecache"
    redis_pid=$(get_redis_pid $host $port)
    cg_dir=/sys/fs/cgroup/memory/${suite}-${setup}-${port}
    if [[ -d $cg_dir ]]; then 
        echo "cgroup mem dir($cg_dir) exists"
    else
        mkdir  $cg_dir
    fi

    cg_pid_file=$cg_dir/cgroup.procs
    cg_limit_file=$cg_dir/memory.limit_in_bytes
    existing_pids=$(cat $cg_pid_file)
    if [[ $existing_pids != "" ]]; then echo "cgroup pid($existing_pids) exists"; return 1; fi

    echo $redis_pid > $cg_pid_file
    echo $((2*1024*1024*1024)) > $cg_limit_file

    echo "limit pagecache result: pid=$(cat $cg_pid_file), limit=$(( $(cat $cg_limit_file)/1024/1024 ))MB"

    echo "setup ok."

    return 0
}

get_case_report() {
    local report_file=$1
    local mode=$2

    if [[ $mode == "" ]]; then mode=human; fi

    if ! [[ -f $report_file ]]; then
        echo "report_file($report_file) not found"
    fi

    if [[ $mode != human && $mode != markdown ]]; then
        echo "mode($mode) no valid"
    fi

    QPS=$(grep QPS $report_file | awk '{print $NF}' | awk -F. '{print $1}')
    final_reports=$(grep -A 55 QPS $report_file)
    lat_mean=$(echo "$final_reports" | grep -A 10 latency | grep mean | awk '{print $NF}' | awk -F. '{print $1}')
    lat_p99=$(echo "$final_reports" | grep -A 10 latency | grep '99%' | awk '{print $NF}' | awk -F. '{print $1}')
    cpu_mean=$(echo "$final_reports" | grep -A 10 cpu | grep mean | awk '{print $NF}' | awk -F. '{print $1}')
    cpu_p99=$(echo "$final_reports" | grep -A 10 cpu | grep '99%' | awk '{print $NF}' | awk -F. '{print $1}')
    mem_mean=$(echo "$final_reports" | grep -A 10 mem | grep mean | awk '{print $NF}' | awk -F. '{print $1}')
    mem_max=$(echo "$final_reports" | grep -A 10 mem | grep 'max' | awk '{print $NF}' | awk -F. '{print $1}')
    disk_read_mean=$(echo "$final_reports" | grep -A 10 diskread | grep mean | awk '{print $NF}' | awk -F. '{print $1}')
    disk_read_p99=$(echo "$final_reports" | grep -A 10 diskread | grep '99%' | awk '{print $NF}' | awk -F. '{print $1}')
    disk_write_mean=$(echo "$final_reports" | grep -A 10 diskwrite | grep mean | awk '{print $NF}' | awk -F. '{print $1}')
    disk_write_p99=$(echo "$final_reports" | grep -A 10 diskwrite | grep '99%' | awk '{print $NF}' | awk -F. '{print $1}')

    mem_mean=$((mem_mean/1024/1024))
    mem_max=$((mem_max/1024/1024))
    disk_read_mean=$((disk_read_mean/1024))
    disk_read_p99=$((disk_read_p99/1024))
    disk_write_mean=$((disk_write_mean/1024))
    disk_write_p99=$((disk_write_p99/1024))

    # QPS：81173<br>延迟(mean,p999): 1206,1753  (us)<br>CPU(mean,p99):  97% 99%<br>内存(mean,max): 10771 10771 (MB)<br>
    if [[ $mode == markdown ]]; then
        echo "QPS=$QPS<br>Latency(mean,p99):$lat_mean $lat_p99(us)<br>CPU(mean,p99):$cpu_mean $cpu_p99<br>Memory(mean,max):$mem_mean $mem_max(MB)<br>DiskWrite(mean,p99):$disk_read_mean $disk_read_p99<MB/s>DiskWrite(mean,p99):$disk_read_mean $disk_read_p99<MB/s>"
    else
        echo \
"QPS=$QPS
Latency(mean,p99):$lat_mean $lat_p99(us)
CPU(mean,p99):$cpu_mean $cpu_p99
Memory(mean,max):$mem_mean $mem_max(MB)
DiskRead(mean,p99):$disk_read_mean $disk_read_p99(MB/s)
DiskWrite(mean,p99):$disk_write_mean $disk_write_p99(MB/s)"
    fi
}

execute_cases() {
    if ! [[ -d $artifact_dir ]]; then mkdir -p $artifact_dir; fi

    for perf_case in ${cases//,/ }
	do
		case_cmd=$(echo $perf_case | awk -F- '{print $1}')
		case_load=$(echo $perf_case | awk -F- '{print $2}')

        echo "execute case: cmd=$case_cmd, load=$case_load start."

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

        case_report_file=$artifact_dir/$log_prefix.$perf_case.log

        $bench2 $host:$port $password $case_cmd $rdb_key_count $key_prefix $case_nthd $case_qps_limit $diskname > $case_report_file

        echo "$log_prefix.$perf_case => $(get_case_report $case_report_file markdown)" >> $markddown_report_file
        case_report=$(get_case_report $case_report_file human)
        echo "execute case: cmd=$case_cmd, load=$case_load finished:"
        echo "=== $log_prefix.$perf_case ==="
        echo "$case_report"
        echo "=============================="

        sleep 10
	done

    return 0
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
    if [[ -z "$base_conf" ]] || ! [[ -f $base_conf ]] ; then echo "base_conf not valid: $base_conf"; return 1; fi
    if [[ -z "$suite"  ]]; then echo "suite notset"; return 1; fi
    if [[ "$suite" != "10G_string" && "$suite" != 10G_hash && "$suite" != 1000M_string ]]; then echo "suite not valid"; return 1; fi
    if [[ -z "$setup" ]]; then echo "setup notset."; return 1; fi
    if [[ "$setup" != cold && "$setup" != warm && "$setup" != hot ]]; then echo "setup not valid"; return 1; fi
    if [[ -z "$cases" ]]; then echo "cases notset."; return 1; fi
    if [[ -z "$diskname" ]]; then echo "diskname notset."; return 1; fi

	key_prefix=$suite
    load_nthd=100
    runner_dir="$base_dir/$runner"
    artifact_dir="$base_dir/$runner/artifacts"
    markddown_report_file=$artifact_dir/markdown_report
    if ! [[ -d $runner_dir ]]; then mkdir -p $runner_dir; fi
    if ! [[ -d $artifact_dir ]]; then mkdir -p $artifact_dir; fi
    conf_file=$runner_dir/redis.conf
    rdb_file="$runner_dir/${suite}.rdb"

    if [[ $suite == 10G_string ]]; then
		val_size=1k
		rdb_write_cmd=set
		rdb_read_cmd=get
        db_mb=10000
    elif [[ $suite == 10G_hash ]]; then
		val_size=2k
		rdb_write_cmd=hmset
		rdb_read_cmd=hgetall
        db_mb=10000
    elif [[ $suite == 1000M_string ]]; then
		val_size=1k
		rdb_write_cmd=set
		rdb_read_cmd=get
        db_mb=1000
    else
		echo "loaded failed: suite $suite not valid"
        return 1
    fi

    if [[ $(iostat | grep $diskname 2>/dev/null) == "" ]]; then echo disk: $diskname not exists.; return 1; fi

	rdb_key_count=$(($db_mb*1000/${val_size/k/}))
    log_prefix=$suite.$setup.$configs

    return 0
}

collect_reports() {
    local tgz_file=$base_dir/$runner.$suite.$setup.$configs.$cases.$(date +%s).tgz
    tar -czf "$tgz_file" $artifact_dir 2>/dev/null
}

# set following env before executing perf:
#### required ####
# host: ror host
# port: ror port
# base_conf: base redis.conf
# runner: different perf runner should run parallel
# suite: 10G_string/10G_hash
# setup: cold/warm/hot
# configs: swap-evict-step-max-subkeys=1000,...
# cases: set-100thd,get-1wqps...
# report: report file
#### optional ####
# base_dir: base directory to place rdb, artifacts, binary
# bench2: path to bench2 binary
# redis_server: path to redis-server binary
# password: redis-server password
# diskname: the disk name to monitor

if [[ "$EUID" != 0 ]]; then 
    echo "Please run as root"
    exit 1
fi

echo "--- current env."
printenv

if ! sanity_check; then
    echo "perf aborted: sanity_check failed."
    exit 1
else
    echo "--- sanity_check ok."
fi

if ! create_conf; then
    echo "perf aborted: create_conf failed."
    exit 1
else
    echo "--- create_conf ok."
fi

if ! create_rdb; then
    echo "perf aborted: create rdb failed."
    exit 1
else
    echo "--- create_rdb ok."
fi

if ! setup_redis; then
    echo "perf aborted: setup redis failed."
    exit 1
else
    echo "--- setup_redis ok."
fi

if ! execute_cases; then
    echo "perf aborted: execute cases failed."
    exit 1
else
    echo "--- execute_cases ok."
fi

if ! collect_reports; then
    echo "perf aborted: collect report failed."
    exit 1
else
    echo "--- collect_reports ok."
fi

echo "--- all succeeded. ---"

