/*
 * Copyright (C) 2016 Intel Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice(s),
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice(s),
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
 * EVENT SHALL THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <memkind/internal/memkind_hbw.h>

#include <numa.h>
#include <stdio.h>

#define MAX_ARG_LEN 8

const char* help_message =
    "\n"
    "NAME\n"
    "    memkind-hbw-nodes - Print comma separated list of high bandwidth nodes.\n"
    "\n"
    "SYNOPSIS\n"
    "    memkind-hbw-nodes -h | --help\n"
    "        Print this help message.\n"
    "\n"
    "DESCRIPTION\n"
    "    Prints a comma separated list of high bandwidth NUMA nodes\n"
    "    that can be used with the numactl --membind option.\n"
    "\n"
    "EXIT STATUS\n"
    "    Return code is :\n"
    "        0 on success\n"
    "        1 on failure\n"
    "        2 on invalid argument\n"
    "\n"
    "COPYRIGHT\n"
    "    Copyright 2016 Intel Corporation All Rights Reserved.\n"
    "\n"
    "AUTHORS\n"
    "    Krzysztof Kulakowski\n"
    "\n"
    "SEE ALSO\n"
    "    hbwmalloc(3), memkind(3)\n"
    "\n";

extern unsigned int numa_bitmask_weight(const struct bitmask *bmp );

int print_hbw_nodes()
{
    int i, j = 0;

    nodemask_t nodemask;
    struct bitmask nodemask_bm = {NUMA_NUM_NODES, nodemask.n};
    numa_bitmask_clearall(&nodemask_bm);

    //WARNING: code below is usage of memkind experimental API which may be changed in future
    if(memkind_hbw_all_get_mbind_nodemask(NULL, nodemask.n, NUMA_NUM_NODES) != 0) {
        return 1;
    }

    for(i=0; i<NUMA_NUM_NODES; i++) {
        if(numa_bitmask_isbitset(&nodemask_bm, i)) {
            printf("%d%s", i, (++j == numa_bitmask_weight(&nodemask_bm)) ? "" : ",");
        }
    }
    printf("\n");
    return 0;
}

int main(int argc, char *argv[])
{
    if(argc == 1) {
        return print_hbw_nodes();
    }
    else if ((argc == 2) && (strncmp(argv[1], "-h", MAX_ARG_LEN) == 0 || strncmp(argv[1], "--help", MAX_ARG_LEN) == 0)) {
        printf("%s", help_message);
        return 2;
    }

    printf("ERROR: Unknown option %s. More info with \"%s --help\".\n", argv[1], argv[0]);
    return 2;
}
