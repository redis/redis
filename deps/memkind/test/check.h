/*
 * Copyright (C) 2014 - 2016 Intel Corporation.
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

#ifndef check_include_h
#define check_include_h

#include <list>
#include "trial_generator.h"


typedef struct {
    unsigned long start_addr;
    unsigned long end_addr;
    size_t pagesize;
} smaps_entry_t;



using namespace std;

class Check
{
public:
    Check(const void *p, const trial_t& trial);
    Check(const void *p, const size_t size, const size_t page_size);
    Check(const Check &);
    ~Check();
    void check_node_hbw();
    void check_node_hbw_interleave();
    int check_page_size(size_t page_size);
    int check_zero(void);
    int check_data(int data);
    int check_align(size_t align);
private:
    const void *ptr;
    size_t size;
    void **address;
    size_t num_address;
    list<smaps_entry_t>smaps_table;
    ifstream ip;
    int smaps_fd;
    string skip_to_next_entry(ifstream &);
    string skip_to_next_kpage(ifstream &);
    void get_address_range(string &line, unsigned long long *start_addr,
                           unsigned long long *end_addr);
    size_t get_kpagesize(string line);
    int check_page_size(size_t page_size, void *vaddr);
    int populate_smaps_table();
};

#endif
