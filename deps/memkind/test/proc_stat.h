/*
 * Copyright (C) 2017 Intel Corporation.
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
#include <iostream>
#include <fstream>
#include "common.h"

namespace ProcStat
{
    std::string get_stat(const std::string& field_name)
    {
        std::string file_name = "/proc/self/status";
        std::ifstream file(file_name);
        std::string line;
        while (std::getline(file, line))
        {
            if (!line.compare(0, field_name.size(), field_name))
            {
                line.erase(0, field_name.size() + 1);
                size_t start = line.find_first_not_of(" \t");
                return line.substr(start);
            }
        }
        return "";
    }

    unsigned extract_leading_number(const std::string& value)
    {
        size_t number_end = value.find_first_not_of("0123456789");
        return std::stoi(value.substr(0, number_end));
    }

    size_t get_virtual_memory_size()
    {
        return extract_leading_number(get_stat("VmSize")) * KB;
    }

    size_t get_physical_memory_size()
    {
        return extract_leading_number(get_stat("VmRSS")) * KB;
    }
}

