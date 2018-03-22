#!/bin/bash
logs_dir=$1
 
get_SET_perc_from_json() {
	local perc=$1
	local file=$2
	cat $file | jq '."ALL STATS".SET|map(select(.percent <= '"$perc"'))|.[-1]."<=msec"'
}
 
get_GET_perc_from_json() {
	local perc=$1
	local file=$2
	cat $file | jq '."ALL STATS".GET|map(select(.percent <= '"$perc"'))|.[-1]."<=msec"'
}
 
get_average_latency_from_json() {
	local file=$1
	cat $file | jq '."ALL STATS".Totals.Latency'
}
 
get_ops_from_json() {
	local file=$1
	cat $file | jq '."ALL STATS".Totals."Ops/sec"'
}
 
n=0
sum_get=0
sum_set=0
sum_lat=0
sum_ops=0
# ignore failed match in case there are no matching files
shopt -s nullglob
for file in $logs_dir/*_bench_*.json; do
	local val_set=$(get_SET_perc_from_json 99 $file)
	local val_get=$(get_GET_perc_from_json 99 $file)
	local val_lat=$(get_average_latency_from_json $file)
	local val_ops=$(get_ops_from_json $file)

	sum_set=`echo "scale=2; $sum_set + $val_set" | bc`
	sum_get=`echo "scale=2; $sum_get + $val_get" | bc`
	sum_lat=`echo "scale=2; $sum_lat + $val_lat" | bc`
	sum_ops=`echo "scale=2; $sum_ops + $val_ops" | bc`
	n=$((n + 1))
done

# Calculate the average of 99th percentiles
pc_set=` echo "scale=2; $sum_set / $n" | bc`
pc_get=` echo "scale=2; $sum_get / $n" | bc`

# Calculate the average latency
avg_lat=` echo "scale=2; $sum_lat / $n" | bc`
 
printf "pc99_GET\tpc99_SET\tavg_LAT\tsum_OPS\tN\n"
printf "$pc_get\t$pc_set\t$avg_lat\t$sum_ops\t$n\n"

