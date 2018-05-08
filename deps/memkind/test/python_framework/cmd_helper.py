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

class CMD_helper(object):

    def execute_cmd(self, command, sudo=False):
        if sudo:
            command = "sudo {0}".format(command)
        #Initialize temp file for stdout. Will be removed when closed.
        outfile = tempfile.SpooledTemporaryFile()
        try:
            #Invoke process
            p = subprocess.Popen(command, stdout=outfile, stderr=subprocess.STDOUT, shell=True)
            p.communicate()
            #Read stdout from file
            outfile.seek(0)
            stdout = outfile.read()
        except:
            raise
        finally:
            #Make sure the file is closed
            outfile.close()
        retcode = p.returncode
        return stdout, retcode

    def get_command_path(self, binary):
        """Get the path to the binary."""
        path = os.path.dirname(os.path.abspath(__file__))
        return os.path.join(path, binary)