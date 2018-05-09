#
#  Copyright (C) 2016 Intel Corporation.
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

import pytest
import os
import tempfile
import subprocess
from python_framework import CMD_helper

class Test_trace_mechanism(object):
    binary = "../trace_mechanism_test_helper"
    fail_msg = "Test failed with:\n {0}"
    debug_env = "MEMKIND_DEBUG=1 "
    cmd_helper = CMD_helper()

    def test_TC_MEMKIND_logging_MEMKIND_HBW(self):
        #This test executes trace_mechanism_test_helper and test if MEMKIND_INFO message occurs while calling MEMKIND_HBW
        command = self.debug_env + self.cmd_helper.get_command_path(self.binary) + " MEMKIND_HBW"
        print "Executing command: {0}".format(command)
        output, retcode = self.cmd_helper.execute_cmd(command, sudo=False)
        assert retcode == 0, self.fail_msg.format("Error: trace_mechanism_test_helper returned {0} with output: {1}".format(retcode,output))
        assert "MEMKIND_INFO: NUMA node" in output, self.fail_msg.format("Error: trace mechanism in memkind doesn't show MEMKIND_INFO message (output: {0})").format(output)

    def test_TC_MEMKIND_2MBPages_logging_MEMKIND_HUGETLB(self):
        #This test executes trace_mechanism_test_helper and test if MEMKIND_INFO message occurs while calling MEMKIND_HUGETLB
        command = self.debug_env + self.cmd_helper.get_command_path(self.binary) + " MEMKIND_HUGETLB"
        print "Executing command: {0}".format(command)
        output, retcode = self.cmd_helper.execute_cmd(command, sudo=False)
        assert retcode == 0, self.fail_msg.format("Error: trace_mechanism_test_helper returned {0} with output: {1}".format(retcode,output))
        assert "MEMKIND_INFO: Number of" in output, self.fail_msg.format("Error: trace mechanism in memkind doesn't show MEMKIND_INFO message (output: {0})").format(output)
        assert "MEMKIND_INFO: Overcommit limit for" in output, self.fail_msg.format("Error: trace mechanism in memkind doesn't show MEMKIND_INFO message (output: {0})").format(output)

    def test_TC_MEMKIND_logging_negative_MEMKIND_DEBUG_env(self):
        #This test executes trace_mechanism_test_helper and test if setting MEMKIND_DEBUG to wrong value causes MEMKIND_WARNING message
        command = "MEMKIND_DEBUG=-1 " + self.cmd_helper.get_command_path(self.binary) + " MEMKIND_HBW"
        print "Executing command: {0}".format(command)
        output, retcode = self.cmd_helper.execute_cmd(command, sudo=False)
        assert retcode == 0, self.fail_msg.format("Error: trace_mechanism_test_helper returned {0} with output: {1}".format(retcode,output))
        assert "MEMKIND_WARNING: debug option" in output, self.fail_msg.format("Error: setting wrong MEMKIND_DEBUG environment variable doesn't show MEMKIND_WARNING (output: {0})").format(output)
