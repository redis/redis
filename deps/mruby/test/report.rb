report
if $ko_test > 0 or $kill_test > 0
  raise "mrbtest failed (KO:#{$ko_test}, Crash:#{$kill_test})"
end
