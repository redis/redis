#!/bin/bash

db_mb=8000
base_dir=/var/lib/k8s/test/bench/xredis-ror
host_port=127.0.0.1:15001
password=nopass
host=$(echo $host_port | awk -F: '{print $1}')
port=$(echo $host_port | awk -F: '{print $2}')
dump_file=$base_dir/dump${port}.rdb
data_rocks=$base_dir/data.rocks
bench2=$base_dir/bin/bench2

# redis_server=$base_dir/bin/redis-server
# export LD_LIBRARY_PATH=/var/lib/k8s/test/test/rocksdb-6.22:$LD_LIBRARY_PATH

redis_server=$base_dir/bin/redis-server-refactor-fixleak
prefix=refactor-fixleak

usage() {
    echo "$0 <string|hash_hget|hash_hgetall> <hot|warm|cold> <100thd|1wqps>"
}

if [[ $# != 3 ]]; then
    usage
    exit 1
fi

bench_type=$1
bench_case=$2
bench_load=$3

if [[ $bench_type != string ]] && [[ $bench_type != hash_hget ]] && [[ $bench_type != hash_hgetall ]]; then
    usage
    exit 1
fi

if [[ $bench_case != hot ]] && [[ $bench_case != cold ]] && [[ $bench_case != warm ]] ; then
    usage
    exit 1
fi

if [[ $bench_load != 100thd ]] && [[ $bench_load != 1wqps ]] ; then
    usage
    exit 1
fi

if [[ "$EUID" != 0 ]]; then 
    echo "Please run as root"
    exit 1
fi

if [[ $bench_type == string ]]; then
    val_size=1k
    bench_write_cmd=set
    bench_read_cmd=get
elif [[ $bench_type == hash_hget ]]; then
    val_size=2k; # 5*400
    bench_write_cmd=hmset
    bench_read_cmd=hget
else
    val_size=2k; # 5*400
    bench_write_cmd=hmset
    bench_read_cmd=hgetall
fi

if [[ $bench_load == 100thd ]]; then
    nthd=100
    qps_limit=0
else
    nthd=1000
    qps_limit=10000
fi

log_prefix=$prefix-$bench_type-$bench_case-$bench_load
key_prefix=$bench_case


wait_redis_up() {
    while true; do
        ping_reply=$(redis-cli -h $host -p $port ping 2>/dev/null | tr -d '[:space:]')
        if [[ $ping_reply != PONG ]]; then echo -n .; sleep 1; else break; fi
    done
}

clean_redis_up() {
    redis-cli -h $host -p $port credis_shutdown nosave 2>/dev/null
    re='/var/lib/k8s/test/.*' # safety check
    if [[ $dump_file =~ $re ]] && [[ -f $dump_file ]]; then rm -rf $dump_file; fi
    if [[ $data_rocks =~ $re ]] && [[ -d $data_rocks ]]; then rm -fr $data_rocks; fi
}

get_maxmemory() {
    local current_memory=$1
    if [[ $bench_case == hot ]]; then
        echo -n "0"
    elif [[ $bench_case == warm ]]; then
        echo -n $((db_mb*1024*1024/4))
    else
        echo -n $((db_mb*1024*1024/4))
    fi
}

echo "=== benching $bench_case $bench_load ==="

# 0. 启动
clean_redis_up
$redis_server redis.conf
wait_redis_up

# 1. 加载数据(8G) 
echo "=== 1. loading ==="
key_count=$(($db_mb*1000/${val_size/k/}))
echo $bench2 $host_port $password $bench_write_cmd $key_count $key_prefix $nthd
$bench2 $host_port $password $bench_write_cmd $key_count $key_prefix $nthd > $log_prefix-write-log

# 2. 剔除数据
echo "=== 2. evicting ==="
redis-cli -h $host -p $port credis_save
sleep 1
redis-cli -h $host -p $port credis_shutdown nosave
$redis_server redis.conf
wait_redis_up

# 3. 设置maxmemory
echo "=== 3. set maxmemory ==="
current_memory=$(redis-cli -h $host -p $port info memory | grep 'used_memory:' | tr -d '[:space:]' | awk -F: '{print $2}')
max_memory=$(get_maxmemory $current_memory)
echo "current_memory=$current_memory, max_memory=$max_memory"
redis-cli -h $host -p $port config set maxmemory $max_memory

# 4. 预热
echo "=== 4. warm up ==="
$bench2 $host_port $password get $(($key_count/5)) $key_prefix $nthd > $log_prefix-warmup-log

# 5. 清空pagecache
echo "=== 5. clean pagecache ==="
sync; echo 1 > /proc/sys/vm/drop_caches

# 6. 限制pagecache
echo "=== 6. limit pagecache ==="
redis_pid=$(redis-cli -h $host -p $port info server | grep process_id | tr -d '[:space:]' | awk -F: '{print $2}')
cg_dir=/sys/fs/cgroup/memory/bench
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

# 7. 执行查询
echo "=== 7. benching ==="
echo $bench2 $host_port $password $bench_read_cmd $key_count $key_prefix $nthd $qps_limit
$bench2 $host_port $password $bench_read_cmd $key_count $key_prefix $nthd $qps_limit > $log_prefix-read-log

# 8. 清理现场
echo "=== 8. cleanup ==="
clean_redis_up

