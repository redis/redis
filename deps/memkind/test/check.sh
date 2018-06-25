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
err=0
basedir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
test_cmd=$basedir/test.sh

# Check if 2MB pages are enabled on system
nr_hugepages=$(cat /proc/sys/vm/nr_hugepages)
nr_overcommit_hugepages=$(cat /proc/sys/vm/nr_overcommit_hugepages)
if [[ "$nr_hugepages" == "0" ]] && [[ "$nr_overcommit_hugepages" == "0" ]]; then
        # Add parameter that disables tests that require 2MB pages
        test_cmd=$test_cmd" -m"
fi

# Check if MCDRAM nodes exists on system
if [ ! -x /usr/bin/memkind-hbw-nodes ]; then
        if [ -x ./memkind-hbw-nodes ]; then
                export PATH=$PATH:$PWD
        else
                echo "Cannot find 'memkind-hbw-nodes' in $PWD. Did you run 'make'?"
                exit 1
        fi
fi
ret=$(memkind-hbw-nodes)
if [[ $ret == "" ]]; then
        # Add parameter that disables tests that detects high bandwidth nodes
        test_cmd=$test_cmd" -d"
fi

if [[ -n $DISABLE_GTEST_TESTS ]]; then
        echo "On demand test disabling detected!"
        test_cmd=$test_cmd" -x $DISABLE_GTEST_TESTS"
fi

if [[ -n "$DISABLE_PYTEST_TESTS" ]]; then
        test_cmd=$test_cmd" -p \"$DISABLE_PYTEST_TESTS\""
        echo "Python test disabling env var detected $DISABLE_PYTEST_TESTS"
fi

eval $test_cmd

err=${PIPESTATUS[0]}

exit $err
