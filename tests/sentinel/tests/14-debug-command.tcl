source "../tests/includes/init-tests.tcl"

test "Sentinel debug test with arguments and without argument" {
   set current_info_period [lindex [S 0 SENTINEL DEBUG] 1]
   S 0 SENTINEL DEBUG info-period 8888
   assert_equal {8888} [lindex [S 0 SENTINEL DEBUG] 1] 
   S 0 SENTINEL DEBUG info-period $current_info_period
   assert_equal $current_info_period [lindex [S 0 SENTINEL DEBUG] 1]
}
