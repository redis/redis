# Create a new thread to dump status periodically.
set server_path [tmpdir "server.status-dump-test"]

proc parse_value {content key} {
  set lines [split $content "\r\n"]
  set value ""
  foreach line $lines {
   if {[regexp {^([^:]*):(.*)$} $line -> k v]} {
     if {$key == $k} {
       set value $v
     }
    }
  }
  return $value
}

start_server [list overrides [list "dir" $server_path]] {
  test "status dumping test on idle server" {
  set info_content [r info]
  set info_lines [split $info_content "\r\n"]
  foreach line $info_lines {
   if {[regexp {^([^:]*):(.*)$} $line -> key value]} {
     if {$key == "run_id"} {
       set info_runid $value
     }
     if {$key == "role"} {
       set info_role $value
     }
     if {$key == "total_commands_processed"} {
       set info_total_commands_processed $value
     }
    }
  }

  # wait a round of status dump.
  after 2000

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp

  set file_lines [split $file_content "\r\n"]

  foreach line $file_lines {
   if {[regexp {^([^:]*):(.*)$} $line -> key value]} {
     if {$key == "run_id"} {
       set file_runid $value
     }
     if {$key == "role"} {
       set file_role $value
     }
     if {$key == "total_commands_processed"} {
       set file_total_commands_processed $value
     }
     if {$key == "report_mstime"} {
       set file_report_mstime $value
     }
     if {$key == "cpu_time"} {
       set file_cpu_time $value
     }
     if {$key == "is_busy"} {
       set file_is_busy $value
     }
     if {$key == "current_command_start_mstime"} {
       set file_current_command_start_mstime $value
     }
    }
  }

  if {$file_runid != $info_runid} {
    fail "expected runid: $info_runid, but: $file_runid"
  }
  if {$file_role != $info_role} {
    fail "expected role: $info_role, but: $file_role"
  }
  set expected_total_commands_processed [expr $info_total_commands_processed + 1]
  if {$file_total_commands_processed != $expected_total_commands_processed} {
    fail "expected total_commands_processed: $expected_total_commands_processed, but: $file_total_commands_processed"
  }
  # check file age.
  set status_file_age [expr [clock milliseconds] - $file_report_mstime]
  set expected_file_age 2000
  if {$status_file_age > $expected_file_age} {
    fail "status file is too old $status_file_age ms, expected younger than $expected_file_age ms"
  }

  set expected_cpu_time 1
  if {$file_cpu_time > $expected_cpu_time} {
    fail "CPU is too busy. cputime is $file_cpu_time, expected less than $expected_cpu_time second"
  }

  set expected_is_busy "false"
  if {$file_is_busy != $expected_is_busy} {
    fail "expected is_busy: $expected_is_busy, but: $file_is_busy"
  }

  set expected_current_command_start_mstime 0
  if {$file_current_command_start_mstime != $expected_current_command_start_mstime} {
    fail "expected current_command_start_mstime: $expected_current_command_start_mstime, but: $file_current_command_start_mstime"
  }
}
}



start_server [list overrides [list "dir" $server_path]] {
  test "status dumping test on busy server" {
  set info_content [r info]
  set info_lines [split $info_content "\r\n"]
  foreach line $info_lines {
   if {[regexp {^([^:]*):(.*)$} $line -> key value]} {
     if {$key == "run_id"} {
       set info_runid $value
     }
     if {$key == "role"} {
       set info_role $value
     }
     if {$key == "total_commands_processed"} {
       set info_total_commands_processed $value
     }
    }
  }

  set rd [redis_deferring_client]
  set current_command_start_mstime [clock milliseconds]
  $rd debug sleep 10.0 ; # Make server unable to reply.

  # wait a round of status dump.
  after 2000

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp

  set file_lines [split $file_content "\r\n"]

  foreach line $file_lines {
   if {[regexp {^([^:]*):(.*)$} $line -> key value]} {
     if {$key == "run_id"} {
       set file_runid $value
     }
     if {$key == "role"} {
       set file_role $value
     }
     if {$key == "total_commands_processed"} {
       set file_total_commands_processed $value
     }
     if {$key == "report_mstime"} {
       set file_report_mstime $value
     }
     if {$key == "cpu_time"} {
       set file_cpu_time $value
     }
     if {$key == "is_busy"} {
       set file_is_busy $value
     }
     if {$key == "current_command_start_mstime"} {
       set file_current_command_start_mstime $value
     }
    }
  }

  if {$file_runid != $info_runid} {
    fail "expected runid: $info_runid, but: $file_runid"
  }
  if {$file_role != $info_role} {
    fail "expected role: $info_role, but: $file_role"
  }
  set expected_total_commands_processed [expr $info_total_commands_processed + 2]
  if {$file_total_commands_processed != $expected_total_commands_processed} {
    fail "expected total_commands_processed: $expected_total_commands_processed, but: $file_total_commands_processed"
  }
  # check file age.
  set status_file_age [expr [clock milliseconds] - $file_report_mstime]
  set expected_file_age 2000
  if {$status_file_age > $expected_file_age} {
    fail "status file is too old $status_file_age ms, expected younger than $expected_file_age ms"
  }

  set expected_cpu_time 1
  if {$file_cpu_time > $expected_cpu_time} {
    fail "CPU is too busy. cputime is $file_cpu_time, expected less than $expected_cpu_time second"
  }

  set expected_is_busy "true"
  if {$file_is_busy != $expected_is_busy} {
    fail "expected is_busy: $expected_is_busy, but: $file_is_busy"
  }

  set expected_current_command_start_mstime_gap 1000
  set current_command_start_mstime_gap [expr {abs([expr $current_command_start_mstime - $file_current_command_start_mstime])}]
  if {$current_command_start_mstime_gap > $expected_current_command_start_mstime_gap} {
    fail "start mstime gap is too big, expected $expected_current_command_start_mstime_gap, actual: $current_command_start_mstime_gap, start time from client: $current_command_start_mstime, from server: $file_current_command_start_mstime"
  }
}
}


start_server [list overrides [list "dir" $server_path "status-dump-interval-sec" 0]] {
  test "test disable status dumping in redis.conf" {
  set info_content [r info]
  set info_lines [split $info_content "\r\n"]
  foreach line $info_lines {
   if {[regexp {^([^:]*):(.*)$} $line -> key value]} {
     if {$key == "run_id"} {
       set info_runid $value
     }
     if {$key == "role"} {
       set info_role $value
     }
     if {$key == "total_commands_processed"} {
       set info_total_commands_processed $value
     }
    }
  }

  set status_file_name [file join $server_path redis.running.status]
  if !{[catch {open $status_file_name r} fp]} {
    set file_content [read $fp]
    close $fp

    set file_lines [split $file_content "\r\n"]

    foreach line $file_lines {
     if {[regexp {^([^:]*):(.*)$} $line -> key value]} {
       if {$key == "run_id"} {
         set file_runid $value
       }
      }
    }

    if {$file_runid == $info_runid} {
      fail "expected different runid with current server instance: $info_runid"
    }
  }

  # delete status file if there is any.
  catch {exec rm -f $status_file_name}
  # wait a round of status dump.
  after 2000

  if !{[catch {open $status_file_name r} fp]} {
    close $fp
    fail "expected no status file on disk if status dump is disabled: $status_file_name"
  }
}
}


start_server [list overrides [list "dir" $server_path "status-dump-interval-sec" 5]] {
  test "test longer status dumping interval in redis.conf" {
  set info_content [r info]

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp

  set info_runid [parse_value $info_content "run_id"]
  set file_runid [parse_value $file_content "run_id"]
  if {$file_runid != $info_runid} {
    fail "expected runid: $info_runid, but: $file_runid"
  }
  set file_report_mstime_first_round [parse_value $file_content "report_mstime"]

  # wait 2 seconds.
  after 2000

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp
  set file_report_mstime_second_round [parse_value $file_content "report_mstime"]
  if {$file_report_mstime_second_round != $file_report_mstime_first_round} {
    fail "expected no status update in 2 seconds as update interval is 5 seconds. first_round: $file_report_mstime_first_round, second_round: $file_report_mstime_second_round"
  }

  # wait another 5 seconds.
  after 4000
  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp
  set file_report_mstime_third_round [parse_value $file_content "report_mstime"]
  if {$file_report_mstime_third_round == $file_report_mstime_first_round} {
    fail "expected status update after 6 seconds as update interval is 5 seconds. first_round: $file_report_mstime_first_round, second_round: $file_report_mstime_second_round, third_round: $file_report_mstime_third_round"
  }
}
}


start_server [list overrides [list "dir" $server_path "status-dump-interval-sec" 1]] {
  test "test enable/disable status dumping in runtime" {
  set info_content [r info]

  assert_equal {1} [lindex [r config get status-dump-interval-sec] 1]

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp

  set info_runid [parse_value $info_content "run_id"]
  set file_runid [parse_value $file_content "run_id"]
  if {$file_runid != $info_runid} {
    fail "expected runid: $info_runid, but: $file_runid"
  }
  set file_report_mstime_first_round [parse_value $file_content "report_mstime"]

  # wait 2 seconds.
  after 2000

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp
  set file_report_mstime_second_round [parse_value $file_content "report_mstime"]
  if {$file_report_mstime_second_round == $file_report_mstime_first_round} {
    fail "expected status update after 2 seconds as update interval is 1 second. first_round: $file_report_mstime_first_round, second_round: $file_report_mstime_second_round"
  }

  # Disable status dump.
  r config set status-dump-interval-sec 0

  assert_equal {0} [lindex [r config get status-dump-interval-sec] 1]

  # delete status file if there is any.
  catch {exec rm -f $status_file_name}

  # wait 2 second
  after 2000

  if !{[catch {open $status_file_name r} fp]} {
    close $fp
    fail "expected no status file on disk if status dump is disabled: $status_file_name"
  }

  # Enable status dump.
  r config set status-dump-interval-sec 1
  assert_equal {1} [lindex [r config get status-dump-interval-sec] 1]

  # wait 20 ms
  after 20

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp

  set info_runid [parse_value $info_content "run_id"]
  set file_runid [parse_value $file_content "run_id"]
  if {$file_runid != $info_runid} {
    fail "expected runid: $info_runid, but: $file_runid"
  }
  set file_report_mstime_first_round [parse_value $file_content "report_mstime"]

  # wait 2s.
  after 2000

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp

  set file_runid [parse_value $file_content "run_id"]
  if {$file_runid != $info_runid} {
    fail "expected runid: $info_runid, but: $file_runid"
  }

  set file_report_mstime_second_round [parse_value $file_content "report_mstime"]
  if {$file_report_mstime_second_round == $file_report_mstime_first_round} {
    fail "expected status update after 2s as update interval is 1s. first_round: $file_report_mstime_first_round, second_round: $file_report_mstime_second_round"
  }

}
}


start_server [list overrides [list "dir" $server_path "status-dump-interval-sec" 1]] {
  test "test change status dumping interval in runtime" {

  assert_equal {1} [lindex [r config get status-dump-interval-sec] 1]

  set info_content [r info]

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp

  set info_runid [parse_value $info_content "run_id"]
  set file_runid [parse_value $file_content "run_id"]
  if {$file_runid != $info_runid} {
    fail "expected runid: $info_runid, but: $file_runid"
  }
  set file_report_mstime_first_round [parse_value $file_content "report_mstime"]

  # wait 2s.
  after 2000

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp
  set file_report_mstime_second_round [parse_value $file_content "report_mstime"]
  if {$file_report_mstime_second_round == $file_report_mstime_first_round} {
    fail "expected status update after 2s as update interval is 1s. first_round: $file_report_mstime_first_round, second_round: $file_report_mstime_second_round"
  }

  # Change status dump interval to 5 sec.
  r config set status-dump-interval-sec 5
  assert_equal {5} [lindex [r config get status-dump-interval-sec] 1]

  # wait 20ms for first round dump.
  after 20

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp

  set info_runid [parse_value $info_content "run_id"]
  set file_runid [parse_value $file_content "run_id"]
  if {$file_runid != $info_runid} {
    fail "expected runid: $info_runid, but: $file_runid"
  }
  set file_report_mstime_first_round [parse_value $file_content "report_mstime"]

  # wait 2s.
  after 2000

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp
  set file_report_mstime_second_round [parse_value $file_content "report_mstime"]
  if {$file_report_mstime_second_round != $file_report_mstime_first_round} {
    fail "expected no status update after 2s as update interval is 5s. first_round: $file_report_mstime_first_round, second_round: $file_report_mstime_second_round"
  }

  # wait another 5 second.
  after 5000

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp
  set file_report_mstime_second_round [parse_value $file_content "report_mstime"]
  if {$file_report_mstime_second_round == $file_report_mstime_first_round} {
    fail "expected status update after 7s as update interval is 5s. first_round: $file_report_mstime_first_round, second_round: $file_report_mstime_second_round"
  }

  # Change status dump interval to 1s.
  r config set status-dump-interval-sec 1
  assert_equal {1} [lindex [r config get status-dump-interval-sec] 1]

  # wait 20ms for first round dump.
  after 20

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp

  set info_runid [parse_value $info_content "run_id"]
  set file_runid [parse_value $file_content "run_id"]
  if {$file_runid != $info_runid} {
    fail "expected runid: $info_runid, but: $file_runid"
  }
  set file_report_mstime_first_round [parse_value $file_content "report_mstime"]

  # wait 2s.
  after 2000

  set fp [open [file join $server_path redis.running.status] r]
  set file_content [read $fp]
  close $fp
  set file_report_mstime_second_round [parse_value $file_content "report_mstime"]
  if {$file_report_mstime_second_round == $file_report_mstime_first_round} {
    fail "expected status update after 2s as update interval is 1s. first_round: $file_report_mstime_first_round, second_round: $file_report_mstime_second_round"
  }
}
}

start_server [list overrides [list "dir" $server_path "status-dump-interval-sec" 100]] {
  test "test invalid status dumping interval in runtime" {

   if !{[catch {r config set status-dump-interval-sec -1}]} {
     fail "expected config failure with invalid value"
   }
  assert_equal {100} [lindex [r config get status-dump-interval-sec] 1]
}
}
