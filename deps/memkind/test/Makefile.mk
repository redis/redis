#
#  Copyright (C) 2014 - 2016 Intel Corporation.
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions are met:
#  1. Redistributions of source code must retain the above copyright notice(s),
#     this list of conditions and the following disclaimer.
#  2. Redistributions in binary form must reproduce the above copyright notice(s),
#     this list of conditions and the following disclaimer in the documentation
#     and/or other materials provided with the distribution.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY EXPRESS
#  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
#  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
#  EVENT SHALL THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
#  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
#  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
#  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
#  OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
#  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

AM_CPPFLAGS += -Itest/gtest_fused -DMEMKIND_DEPRECATED\(x\)=x

check_PROGRAMS += test/all_tests \
                  test/environ_err_hbw_malloc_test \
                  test/decorator_test \
                  test/allocator_perf_tool_tests \
                  test/autohbw_test_helper \
                  test/trace_mechanism_test_helper \
                  test/gb_page_tests_bind_policy \
                  test/gb_page_tests_preferred_policy \
                  test/bat_bind_tests \
                  test/bat_interleave_tests \
                  # end

TESTS += test/check.sh

EXTRA_DIST += test/memkind-afts.ts \
              test/memkind-afts-ext.ts \
              test/memkind-slts.ts \
              test/memkind-perf.ts \
              test/memkind-perf-ext.ts \
              test/memkind-pytests.ts \
              test/hbw_detection_test.py \
              test/autohbw_test.py \
              test/trace_mechanism_test.py \
              test/python_framework/cmd_helper.py \
              test/python_framework/__init__.py \
              test/draw_plots.py \
              test/run_alloc_benchmark.sh \
              test/gtest_fused/gtest/gtest-all.cc \
              test/gtest_fused/gtest/gtest.h \
              # end



test_all_tests_LDADD = libmemkind.la
test_environ_err_hbw_malloc_test_LDADD = libmemkind.la
test_decorator_test_LDADD = libmemkind.la
test_allocator_perf_tool_tests_LDADD = libmemkind.la
test_autohbw_test_helper_LDADD = libmemkind.la
test_gb_page_tests_bind_policy_LDADD = libmemkind.la
test_gb_page_tests_preferred_policy_LDADD = libmemkind.la
test_bat_bind_tests_LDADD = libmemkind.la
test_bat_interleave_tests_LDADD = libmemkind.la
test_trace_mechanism_test_helper_LDADD = libmemkind.la

fused_gtest = test/gtest_fused/gtest/gtest-all.cc \
              test/main.cpp \
              # end

test_all_tests_SOURCES = $(fused_gtest) \
                         test/common.h \
                         test/bat_tests.cpp \
                         test/trial_generator.cpp \
                         test/check.cpp \
                         test/multithreaded_tests.cpp \
                         test/trial_generator.h \
                         test/static_kinds_list.h \
                         test/check.h \
                         test/extended_tests.cpp \
                         test/negative_tests.cpp \
                         test/error_message_tests.cpp \
                         test/partition_tests.cpp \
                         test/get_size_tests.cpp \
                         test/create_tests.cpp \
                         test/create_tests_helper.c \
                         test/memkind_default_tests.cpp \
                         test/policy_tests.cpp \
                         test/get_arena_test.cpp \
                         test/memkind_pmem_tests.cpp \
                         test/performance/operations.hpp \
                         test/performance/perf_tests.hpp \
                         test/performance/perf_tests.cpp \
                         test/performance/framework.hpp \
                         test/performance/framework.cpp \
                         test/hbw_allocator_tests.cpp \
                         test/memkind_versioning_tests.cpp \
                         test/new_kind_test.cpp \
                         test/numakind_test.cpp \
                         test/static_kinds_tests.cpp \
                         test/hbw_verify_function_test.cpp \
                         #end

test_environ_err_hbw_malloc_test_SOURCES = test/environ_err_hbw_malloc_test.cpp
test_decorator_test_SOURCES = $(fused_gtest) test/decorator_test.cpp test/decorator_test.h
test_autohbw_test_helper_SOURCES = test/autohbw_test_helper.c
test_gb_page_tests_bind_policy_SOURCES = $(fused_gtest) test/gb_page_tests_bind_policy.cpp test/trial_generator.cpp test/check.cpp
test_gb_page_tests_preferred_policy_SOURCES = $(fused_gtest) test/gb_page_tests_preferred_policy.cpp test/trial_generator.cpp test/check.cpp
test_bat_bind_tests_SOURCES = $(fused_gtest) test/bat_bind_tests.cpp test/trial_generator.cpp test/check.cpp
test_bat_interleave_tests_SOURCES = $(fused_gtest) test/bat_interleave_tests.cpp test/trial_generator.cpp test/check.cpp
test_trace_mechanism_test_helper_SOURCES = test/trace_mechanism_test_helper.c

#Tests based on Allocator Perf Tool
allocator_perf_tool_library_sources = test/allocator_perf_tool/AllocationSizes.hpp \
                                      test/allocator_perf_tool/Allocation_info.hpp \
                                      test/allocator_perf_tool/Allocation_info.cpp \
                                      test/allocator_perf_tool/Allocator.hpp \
                                      test/allocator_perf_tool/AllocatorFactory.hpp \
                                      test/allocator_perf_tool/CSVLogger.hpp \
                                      test/allocator_perf_tool/CommandLine.hpp \
                                      test/allocator_perf_tool/Configuration.hpp \
                                      test/allocator_perf_tool/ConsoleLog.hpp \
                                      test/allocator_perf_tool/FootprintSampling.cpp \
                                      test/allocator_perf_tool/FootprintSampling.h \
                                      test/allocator_perf_tool/FootprintTask.cpp \
                                      test/allocator_perf_tool/FootprintTask.h \
                                      test/allocator_perf_tool/FunctionCalls.hpp \
                                      test/allocator_perf_tool/FunctionCallsPerformanceTask.cpp \
                                      test/allocator_perf_tool/FunctionCallsPerformanceTask.h \
                                      test/allocator_perf_tool/GTestAdapter.hpp \
                                      test/allocator_perf_tool/Iterator.hpp \
                                      test/allocator_perf_tool/JemallocAllocatorWithTimer.hpp \
                                      test/allocator_perf_tool/MemkindAllocatorWithTimer.hpp \
                                      test/allocator_perf_tool/MemoryFootprintStats.hpp \
                                      test/allocator_perf_tool/Numastat.hpp \
                                      test/allocator_perf_tool/Runnable.hpp \
                                      test/allocator_perf_tool/Sample.hpp \
                                      test/allocator_perf_tool/ScenarioWorkload.cpp \
                                      test/allocator_perf_tool/ScenarioWorkload.h \
                                      test/allocator_perf_tool/StandardAllocatorWithTimer.hpp \
                                      test/allocator_perf_tool/Stats.hpp \
                                      test/allocator_perf_tool/StressIncreaseToMax.cpp \
                                      test/allocator_perf_tool/StressIncreaseToMax.h \
                                      test/allocator_perf_tool/Task.hpp \
                                      test/allocator_perf_tool/TaskFactory.hpp \
                                      test/allocator_perf_tool/Tests.hpp \
                                      test/allocator_perf_tool/Thread.hpp \
                                      test/allocator_perf_tool/TimerSysTime.hpp \
                                      test/allocator_perf_tool/VectorIterator.hpp \
                                      test/allocator_perf_tool/Workload.hpp \
                                      test/allocator_perf_tool/WrappersMacros.hpp \
                                      test/allocator_perf_tool/HugePageUnmap.hpp \
                                      test/allocator_perf_tool/HugePageOrganizer.hpp \
                                      # end


test_allocator_perf_tool_tests_SOURCES = $(allocator_perf_tool_library_sources) \
                                         $(fused_gtest) \
                                         test/allocate_to_max_stress_test.cpp \
                                         test/heap_manager_init_perf_test.cpp \
                                         test/huge_page_test.cpp \
                                         test/alloc_performance_tests.cpp \
                                         # end


test_allocator_perf_tool_tests_CPPFLAGS = -Itest/allocator_perf_tool/ -lpthread -lnuma -O0 -Wno-error $(AM_CPPFLAGS)
test_allocator_perf_tool_tests_CXXFLAGS = -Itest/allocator_perf_tool/ -lpthread -lnuma -O0 -Wno-error $(AM_CPPFLAGS)
if ENABLE_CXX11
test_allocator_perf_tool_tests_CPPFLAGS += -std=c++11
test_allocator_perf_tool_tests_CXXFLAGS += -std=c++11
endif

NUMAKIND_MAX = 2048
test_all_tests_CXXFLAGS = $(AM_CXXFLAGS) $(CXXFLAGS) $(OPENMP_CFLAGS) -DNUMAKIND_MAX=$(NUMAKIND_MAX)

#Allocator Perf Tool stand alone app
check_PROGRAMS += test/perf_tool
test_perf_tool_LDADD = libmemkind.la
test_perf_tool_SOURCES = $(allocator_perf_tool_library_sources) \
                         test/allocator_perf_tool/main.cpp \
                         # end


test_perf_tool_CPPFLAGS = -Itest/allocator_perf_tool/ -lpthread -lnuma -O0 -Wno-error $(AM_CPPFLAGS)
test_perf_tool_CXXFLAGS = -Itest/allocator_perf_tool/ -lpthread -lnuma -O0 -Wno-error $(AM_CPPFLAGS)
if ENABLE_CXX11
test_perf_tool_CPPFLAGS += -std=c++11
test_perf_tool_CXXFLAGS += -std=c++11
endif

#Alloc benchmark
check_PROGRAMS += test/alloc_benchmark_hbw \
                  test/alloc_benchmark_glibc \
                  test/alloc_benchmark_tbb \
                  # end

test_alloc_benchmark_hbw_LDADD = libmemkind.la
test_alloc_benchmark_hbw_SOURCES = test/alloc_benchmark.c
test_alloc_benchmark_hbw_CFLAGS = -O0 -g -fopenmp -Wall -DHBWMALLOC -lmemkind

test_alloc_benchmark_glibc_LDADD = libmemkind.la
test_alloc_benchmark_glibc_SOURCES = test/alloc_benchmark.c
test_alloc_benchmark_glibc_CFLAGS = -O0 -g -fopenmp -Wall

test_alloc_benchmark_tbb_LDADD = libmemkind.la
test_alloc_benchmark_tbb_SOURCES = test/alloc_benchmark.c \
                                   test/load_tbbmalloc_symbols.c \
                                   test/tbbmalloc.h \
                                   # end
test_alloc_benchmark_tbb_CFLAGS = -O0 -g -fopenmp -Wall -DTBBMALLOC -ldl

# Examples as tests
check_PROGRAMS += test/hello_memkind \
                  test/autohbw_candidates \
                  test/hello_memkind_debug \
                  test/hello_hbw \
                  test/filter_memkind \
                  test/gb_realloc \
                  test/pmem \
                  # end
if ENABLE_CXX11
check_PROGRAMS += test/memkind_allocated
endif


test_hello_memkind_LDADD = libmemkind.la
test_hello_memkind_debug_LDADD = libmemkind.la
test_hello_hbw_LDADD = libmemkind.la
test_filter_memkind_LDADD = libmemkind.la
test_gb_realloc_LDADD = libmemkind.la
test_pmem_LDADD = libmemkind.la
test_autohbw_candidates_LDADD = libmemkind.la \
                                # end
if ENABLE_CXX11
test_memkind_allocated_LDADD = libmemkind.la
endif

test_hello_memkind_SOURCES = examples/hello_memkind_example.c
test_hello_memkind_debug_SOURCES = examples/hello_memkind_example.c examples/memkind_decorator_debug.c
test_hello_hbw_SOURCES = examples/hello_hbw_example.c
test_filter_memkind_SOURCES = examples/filter_example.c
test_gb_realloc_SOURCES = examples/gb_realloc_example.c
test_pmem_SOURCES = examples/pmem_example.c
test_autohbw_candidates_SOURCES = examples/autohbw_candidates.c
test_libautohbw_la_SOURCES = autohbw/autohbw.c
noinst_LTLIBRARIES += test/libautohbw.la
if ENABLE_CXX11
test_memkind_allocated_SOURCES = examples/memkind_allocated_example.cpp examples/memkind_allocated.hpp
endif

CLEANFILES += test/numakind_macro.h
test/numakind_test.cpp: test/numakind_macro.h
test/numakind_macro.h:
	for ((i=0;i<$(NUMAKIND_MAX);i++)); do \
		echo "NUMAKIND_GET_MBIND_NODEMASK_MACRO($$i)" >> $@; \
		done
	echo 'const struct memkind_ops NUMAKIND_OPS[NUMAKIND_MAX] = {' >> $@
	for ((i=0;i<$(NUMAKIND_MAX);i++)); do \
		echo "NUMAKIND_OPS_MACRO($$i)," >> $@; \
		done
	echo '};' >> $@
	echo >> $@

