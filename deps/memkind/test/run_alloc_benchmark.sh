#!/bin/bash
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

# Allocator
ALLOCATOR="glibc tbb hbw"
# Thread configuration
THREADS=(1 2 4 8 16 18 36)
# Memory configuration (in kB) / iterations
MEMORY=(1 4 16 64 256 1024 4096 16384)
# Iterations
ITERS=1000

export KMP_AFFINITY=scatter,granularity=fine

# For each algorithm
for alloc in $ALLOCATOR
do
    rm -f alloctest_$alloc.txt
    echo "#Number of threads, allocation size [kB], total time [s], allocation time [s], \
free time [s], first allocation time [s], first free time [s]" >> alloctest_$alloc.txt
    # For each number of threads
    for nthr in ${THREADS[*]}
    do
        # For each amount of memory
        for mem in ${MEMORY[*]}
        do
            echo "OMP_NUM_THREADS=$nthr ./alloc_benchmark_$alloc $ITERS $mem >> alloctest_$alloc.txt"
            OMP_NUM_THREADS=$nthr ./alloc_benchmark_$alloc $ITERS $mem >> alloctest_$alloc.txt
            ret=$?
            if [ $ret -ne 0 ]; then
                echo "Error: alloc_benchmark_$alloc returned $ret"
                exit
            fi
        done
    done
done

echo "Data collected. You can draw performance plots using python draw_plots.py."
