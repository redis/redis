#!/usr/bin/env bash

# fd 3 is meant to catch the actual access to /proc/pid/fd, 
# in case there's an fd leak by the sentinel,
# it can take 3, but then the access to /proc will take another fd, and we'll catch that.

OS=`uname -s`

sentinel_fd_leaks_file="../sentinel_fd_leaks"

if [ ${OS} == "Linux" ]
then
    leaked_fd_count=`ls /proc/self/fd | grep -vE '^[0|1|2|3]$' | wc -l`
    if [ $leaked_fd_count -gt 0 ]
    then
        if [ ! -f $sentinel_fd_leaks_file ]
        then
            ls -l /proc/self/fd | cat >> $sentinel_fd_leaks_file
        fi
    fi
else
    echo "OS: expected Linux, but got ${OS}."
fi


    
