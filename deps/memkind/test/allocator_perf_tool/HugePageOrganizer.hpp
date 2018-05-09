
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
#include <fstream>
#include <string.h>

#define ASSERT_HUGEPAGES_AVAILABILITY() ASSERT_TRUE(HugePageOrganizer::get_nr_hugepages() > 0) << "No hugepages (2MB pages) found.";
#define ASSERT_GBPAGES_AVAILABILITY() ASSERT_TRUE(HugePageOrganizer::get_nr_1GB_pages() > 0) << "No gigabyte pages found.";

class HugePageOrganizer
{

public:

    static bool get_nr_1GB_pages()
    {
        std::string line;
        std::ifstream file("/sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages");
        if (!file.is_open()) {
            return -1;
        }
        std::getline(file, line);
        return strtol(line.c_str(), 0, 10);
    }

    static int get_nr_hugepages()
    {
        std::string line;
        std::ifstream file("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages");
        if (!file.is_open()) {
            return -1;
        }
        std::getline(file, line);
        return strtol(line.c_str(), 0, 10);
    }

    static int set_nr_hugepages(int nr_hugepages)
    {
        char cmd[128];
        sprintf(cmd, "sudo sysctl vm.nr_hugepages=%d", nr_hugepages);
        return system(cmd);
    }
};

