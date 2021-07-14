source "../tests/includes/init-tests.tcl"

proc check_sentinel_port_collision {} {
	#make sure no two sentinel has same port
	foreach_sentinel_id id {
		set sentinel_ports {}
		foreach sentinel [S $id SENTINEL SENTINELS mymaster] {
			set sentinel_port [dict get $sentinel port]
		    if {$sentinel_port in $sentinel_ports} {
		        fail "Sentinel port collision detected, port number is $sentinel_port."
		    } 
		    lappend sentinel_ports $sentinel_port
		}
	}
}

test "Start/Stop sentinel on same port should not cause port collision in other instances view" {
	set sentinel_id [expr $::instances_count-1]
	set org_runid [S $sentinel_id SENTINEL MYID]

	kill_instance sentinel $sentinel_id

	after 5000

	#replace with a different runid in config file, to simulate a new sentinel instance with 
	#same port
	set orgfilename [file join "sentinel_$sentinel_id" "sentinel.conf"]
	set tmpfilename "sentinel.conf_tmp"
	delete_lines_with_pattern $orgfilename $tmpfilename "myid"

	restart_instance sentinel $sentinel_id

	after 5000

	check_sentinel_port_collision

	kill_instance sentinel $sentinel_id

	after 5000

	delete_lines_with_pattern $orgfilename $tmpfilename "myid"

	set config_myid_line  "sentinel myid $org_runid"
	write_line_into_file $orgfilename $config_myid_line

	restart_instance sentinel $sentinel_id

	after 5000

	check_sentinel_port_collision
}