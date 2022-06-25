#!/bin/bash

# echo "case 1 start"
# ./bench_ror.sh string hot 100thd >> run.log 2>&1
# echo "======="
# sleep 100

# echo "case 2"
# ./bench_ror.sh string hot 1wqps >> run.log 2>&1
# echo "======="
# sleep 100

# echo "case 3"
# ./bench_ror.sh string cold 100thd >> run.log 2>&1
# echo "======="
# sleep 100

# echo "case 4"
# ./bench_ror.sh string cold 1wqps >> run.log 2>&1

# echo "case 5"
# ./bench_ror.sh hash_hget hot 100thd >> run.log 2>&1
# echo "======="
# sleep 100

# echo "case 6"
# ./bench_ror.sh hash_hget hot 1wqps >> run.log 2>&1
# echo "======="
# sleep 100

echo "case 7"
./bench_ror.sh hash_hgetall hot 100thd >> run.log 2>&1
echo "======="
sleep 100

echo "case 8"
./bench_ror.sh hash_hgetall hot 1wqps >> run.log 2>&1
echo "======="
sleep 100
