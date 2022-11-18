source "../tests/includes/init-tests.tcl"

test "Sentinel debug test with arguments and without argument" {
   set current_info_period [lindex [S 0 SENTINEL DEBUG] 1]
   S 0 SENTINEL DEBUG info-period 8888
   assert { [lindex [S 0 SENTINEL DEBUG] 1] == {8888} }
   S 0 SENTINEL DEBUG info-period $current_info_period
   assert { [lindex [S 0 SENTINEL DEBUG] 1] == $current_info_period }
}
