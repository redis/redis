#!/bin/bash
set -e

show_help() {
cat << EOF  
Usage: ./aof-backup [OPTIONS] backupfile.tar.gz
Backup AOF files of redis instance

-h <hostname>   Redis host to connect to (must be local on this machine)
-p <port>       Redis port to connect to (must be local on this machine)
-s <unixsocket> Unixsock path to connect to
-u <username>   Redis server username. Needs -a.
-a <password>   Redis server password.

backupfile.tar.gz gzip file to create backup in
EOF
}

cmd=redis-cli
if ! which $cmd > /dev/null; then
    echo "redis-cli not found in path, aborting..."
    exit 1
fi

while getopts 'h:p:s:u:a:' opt; do
    case $opt in
        h) cmd="$cmd -h $OPTARG";;
        p) cmd="$cmd -p $OPTARG";;
        s) cmd="$cmd -s $OPTARG";;
        u) cmd="$cmd --user $OPTARG";;
        a) cmd="$cmd --pass $OPTARG";;
        ?)
            show_help
            exit 1
        ;;
    esac
done

if [ $? -ne 0 ]; then
    echo "Invalid arguments"
    show_help
    exit 1
fi    

if [ ! $# -eq $OPTIND ]; then
    echo "Missing backup filename argument."
    show_help
    exit 1
else
    filename=${@:$OPTIND:1}
fi


get_info_field() {
    $cmd info all | grep $1 | cut -d ":" -f 2 | tr -d '\r\n'
}

get_config() {
    $cmd config get $1 | tail -n 1
}

if [ "$($cmd ping)" != "PONG" ]; then
    echo "Error running redis-cli, check arguments, aborting..."
    exit 1
fi

if [ $(get_config appendonly) != yes ]; then
    echo "Redis not configured for append only, aborting..."
    exit 1
fi    

pid=$(get_info_field process_id)
appenddirname=$(get_config appenddirname)
appenddir_path=$(readlink /proc/$pid/cwd)/$appenddirname
if [ ! -d $appenddir_path ]; then
    echo "Couldn't find $appenddir_path. redis-server must be run locally. Aborting."
    exit 1
fi

while true; do
    aof_rewrites=$(get_info_field aof_rewrites)
    if [ $(get_info_field "aof_rewrite_in_progress") -eq 1 ]; then
      echo "Redis is performing an AOF rewrite, waiting..."
      sleep 1
      continue
    fi

    tmp_dir=$(mktemp -d)
    backup_path=$tmp_dir/$appenddirname
    mkdir $backup_path

    ln $appenddir_path/* $backup_path
    
    if [ $(get_info_field aof_rewrites) -eq $aof_rewrites ]; then break; fi
    
    echo "Encountered a rewrite during backup, retrying..."
    rm -r $tmp_dir
done

tar -czf $filename -C $tmp_dir --warning no-file-change $appenddirname  || [[ $? -eq 1 ]]
rm -r $tmp_dir
echo "done, succfully created backup file $filename"

