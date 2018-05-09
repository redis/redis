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

from distutils.spawn import find_executable
from python_framework import CMD_helper

class Test_hbw_detection(object):
    binary_path = find_executable("memkind-hbw-nodes")
    environ_err_test = "../environ_err_hbw_malloc_test"
    expected_libnuma_warning = "libnuma: Warning: node argument -1 is out of range\n\n"
    fail_msg = "Test failed with:\n {0}"
    cmd_helper = CMD_helper()

    def get_nodemask_default(self):
        """ This function executes memkind function 'get_mbind_nodemask' and returns its output """
        hbw_nodemask_default = None
        command = self.cmd_helper.get_command_path(self.binary_path)
        output, retcode = self.cmd_helper.execute_cmd(command, sudo=False)
        if retcode == 0:
            hbw_nodemask_default = output
            print "Nodemask detected in test_hbw_detection_default: {0}".format(hbw_nodemask_default)
        assert retcode == 0, self.fail_msg.format("Error: hbw_nodemask returned {0}".format(retcode))
        return hbw_nodemask_default

    def get_nodemask_env_variable(self):
        """ This function overrides environment variable MEMKIND_HBW_NODES with values returned from 'memkind-hbw-nodes',
        executes memkind function 'get_mbind_nodemask' and returns its output """
        hbw_nodemask_env_variable = None
        command = "MEMKIND_HBW_NODES=`memkind-hbw-nodes` " + self.cmd_helper.get_command_path(self.binary_path)
        output, retcode = self.cmd_helper.execute_cmd(command, sudo=False)
        if retcode == 0:
            hbw_nodemask_env_variable = output
            print "Nodemask detected in test_hbw_detection_env_variable: {0}".format(hbw_nodemask_env_variable)
        assert retcode == 0, self.fail_msg.format("Error: hbw_nodemask returned {0}".format(retcode))
        return hbw_nodemask_env_variable

    def test_TC_MEMKIND_hbw_detection_default(self):
        """ This test checks whether hbw_nodemask_default is not None """
        assert self.get_nodemask_default() is not None, self.fail_msg.format("Error: hbw_nodemask_default is None")

    def test_TC_MEMKIND_hbw_detection_env_variable(self):
        """ This test checks whether hbw_nodemask_env_variable is not None """
        assert self.get_nodemask_env_variable() is not None, self.fail_msg.format("Error: hbw_nodemask_env_variable is None")

    def test_TC_MEMKIND_hbw_detection_compare_nodemask_default_and_env_variable(self):
        """ This test checks whether hbw_nodemask_default and hbw_nodemask_env_variable has the same value """
        hbw_nodemask_default = self.get_nodemask_default()
        hbw_nodemask_env_variable = self.get_nodemask_env_variable()
        assert hbw_nodemask_default == hbw_nodemask_env_variable, self.fail_msg.format("Error: Nodemask hbw_nodemask_default ({0}) " \
               "is not the same as nodemask hbw_nodemask_env_variable ({1})".format(hbw_nodemask_default, hbw_nodemask_env_variable))

    def test_TC_MEMKIND_hbw_detection_negative_hbw_malloc(self):
        """ This test sets usupported value of MEMKIND_HBW_NODES, then try to perform a successfull allocation from DRAM using hbw_malloc()
        thanks to default HBW_POLICY_PREFERRED policy """
        command = "MEMKIND_HBW_NODES=-1 " + self.cmd_helper.get_command_path(self.environ_err_test)
        output, retcode = self.cmd_helper.execute_cmd(command, sudo=False)
        assert retcode != 0, self.fail_msg.format("Error: hbw_nodemask returned {0} with output {1}".format(retcode, output))
        assert self.expected_libnuma_warning == output, self.fail_msg.format("Error: expected libnuma warning ({0}) " \
               "was not found (output: {1})").format(self.expected_libnuma_warning, output)
