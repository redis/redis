#!/usr/bin/perl
#
#  Copyright (C) 2015 - 2016 Intel Corporation.
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

use strict;

my $usage = "Usage: get_autohbw_srclines.pl output_log_of_AutoHBW executable";

# Check for 2 arguments
#
if (@ARGV != 2) {
    print $usage, "\n";
    exit;
}

# Read the command line arguments
#
my $LogF = shift @ARGV;
my $BinaryF = shift @ARGV;

&main();

sub main {


    print("Info: Reading AutoHBW log from: $LogF\n");
    print("Info: Binary file: $BinaryF\n");

    # open the log file produced by AutoHBM and look at lines starting
    # with Log
    open LOGF, "grep Log $LogF |" or die "Can't open log file $LogF";

    my $line;

    # Read each log line
    #
    while ($line = <LOGF>) {

        # if the line contain 3 backtrace addresses, try to find the source
        # lines for them
        #
        if ($line =~ /^(Log:.*)Backtrace:.*0x([0-9a-f]+).*0x([0-9a-f]+).*0x([0-9a-f]+)/ ) {

            #  Read the pointers
            #
            my @ptrs;

            my $pre = $1;

            $ptrs[0] = $2;
            $ptrs[1] = $3;
            $ptrs[2] = $4;

            # prints the first portion of the line
            #
            print $pre, "\n";

            # for each of the pointers, lookup its source line using
            # addr2line and print the src line(s) if found
            #
            my $i=0;
            for ($i=0; $i < @ptrs; $i++) {

                my $addr = $ptrs[$i];

                open SRCL, "addr2line -e $BinaryF 0x$addr |"
                    or die "addr2line fail";


                my $srcl = <SRCL>;

                if ($srcl =~ /^\?/) {

                } else {

                    print "\t- Src: $srcl";
                }

            }

        }

    }

}
