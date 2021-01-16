#!/bin/sh

leaked_fd_count=`ls /proc/self/fd | grep -vE '0|1|2|3' | wc -l`

sentinel_fd_leaks_file="../sentinel_fd_leaks"

if [ $leaked_fd_count -gt 0 ]
then
    # echo -e '\nwarning: there are leaked fds in sentinel, please fix it:'
    # echo 'Process ID:' $$
    # lsof -p $$
    ls -l /proc/self/fd
    ls -l /proc/self/fd | cat >> $sentinel_fd_leaks_file
fi
    
