
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
#pragma once

#include <fstream>
#include <string.h>
#include <numa.h>
#include <vector>
#include <iostream>
#include <stdexcept>

/*
 * HugePageOrganizer sets hugepages per NUMA node and restore initial setup of hugepages.
 * It writes and reads from the same file, so using HugePageOrganizer with parallel execution may cause undefined behaviour.
 */

class HugePageOrganizer
{
public:
    HugePageOrganizer(int nr_hugepages_per_node)
    {
        for (int node_id = 0; node_id < numa_num_configured_nodes(); node_id++) {
            initial_nr_hugepages_per_nodes.push_back(get_nr_hugepages(node_id));
            if (set_nr_hugepages(nr_hugepages_per_node, node_id)) {
                restore();
                throw std::runtime_error("Error: Could not set the requested amount of huge pages.");
            }
        }
    }

    ~HugePageOrganizer()
    {
        restore();
    }
private:
    std::vector<int> initial_nr_hugepages_per_nodes;

    int get_nr_hugepages(int node_number)
    {
        std::string line;
        char path[128];
        sprintf(path, "/sys/devices/system/node/node%d/hugepages/hugepages-2048kB/nr_hugepages", node_number);
        std::ifstream file(path);
        if (!file) {
            return -1;
        }
        std::getline(file, line);
        return strtol(line.c_str(), 0, 10);
    }

    int set_nr_hugepages(int nr_hugepages, int node_number)
    {
        char cmd[128];
        sprintf(cmd, "sudo sh -c \"echo %d > \
            /sys/devices/system/node/node%d/hugepages/hugepages-2048kB/nr_hugepages\"",
            nr_hugepages, node_number);
        if (system(cmd) || (get_nr_hugepages(node_number) != nr_hugepages)) {
            return -1;
        }
        return 0;
    }

    void restore()
    {
        for(size_t node_id = 0; node_id < initial_nr_hugepages_per_nodes.size(); node_id++) {
            set_nr_hugepages(initial_nr_hugepages_per_nodes[node_id], node_id);
        }
    }
};