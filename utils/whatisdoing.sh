# This script is from http://poormansprofiler.org/
#
# NOTE: Instead of using this script, you should use the Redis
# Software Watchdog, which provides a similar functionality but in
# a more reliable / easy to use way.
#
# Check http://redis.io/topics/latency for more information.

#!/bin/bash
nsamples=1
sleeptime=0
pid=$(ps auxww | grep '[r]edis-server' | awk '{print $2}')

for x in $(seq 1 $nsamples)
  do
    gdb -ex "set pagination 0" -ex "thread apply all bt" -batch -p $pid
    sleep $sleeptime
  done | \
awk '
  BEGIN { s = ""; } 
  /Thread/ { print s; s = ""; } 
  /^\#/ { if (s != "" ) { s = s "," $4} else { s = $4 } } 
  END { print s }' | \
sort | uniq -c | sort -r -n -k 1,1
