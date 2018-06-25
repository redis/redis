/usr/share/mpss/test/memkind-dt/allocator_perf_tool_tests -a --gtest_filter=AllocateToMaxStressTests*:-HugePageTest*
/usr/share/mpss/test/memkind-dt/allocator_perf_tool_tests -a --gtest_filter=HugePageTest* --gtest_repeat=10 --gtest_break_on_failure
