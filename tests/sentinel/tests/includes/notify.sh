#!/bin/sh

leaked_fd_count=`ls /proc/self/fd | grep -vE '0|1|2' | wc -l`

if [ $leaked_fd_count -gt 0 ]
then
    echo -e '\nthere are leaked fds in sentinel, please fix it:'
    echo 'Process ID:' $$
    ls -l /proc/self/fd
    lsof -p $$
    exit 1
else
    echo 'fd leaks test passes.'
fi
    
