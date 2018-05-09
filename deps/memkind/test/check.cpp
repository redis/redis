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

#include <memkind/internal/memkind_hbw.h>

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <numaif.h>
#include <errno.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <numa.h>

#include "check.h"


using namespace std;


/*Check each page between the start
and the end address additionally also
check the end address for pagesize*/
Check::Check(const void *p, const trial_t &trial): Check(p, trial.size, trial.page_size)
{
}

Check::Check(const void *p, const size_t size, const size_t page_size)
{
    const size_t min_page_size = 4096;
    this->ptr = p;
    this->size = size;
    size_t psize = (page_size >= min_page_size ? page_size : min_page_size);
    if (p && size && psize) {
        num_address = size / psize;
        num_address += size % psize ? 1 : 0;

        address = new void* [num_address];
        size_t i;
        for (i = 0; i < num_address - 1; ++i) {
            address[i] = (char *)ptr + i * psize;
        }
        address[i] = (char *)p + size - 1;
    }
    else {
        address = NULL;
        num_address = 0;
    }
}

Check::~Check()
{
    delete[] address;
}

Check::Check(const Check &other)
{
    num_address = other.num_address;

    address = new void* [num_address];
    for (size_t i = 0; i < num_address; ++i) {
        address[i] = other.address[i];
    }
}

void Check::check_node_hbw()
{
    int status = -1;
    struct bitmask *expected_nodemask = numa_allocate_nodemask(), *returned_nodemask = numa_allocate_nodemask();

    memkind_hbw_all_get_mbind_nodemask(NULL, expected_nodemask->maskp, expected_nodemask->size);

    for (size_t i = 0; i < num_address; i++) {
        ASSERT_EQ(get_mempolicy(&status, returned_nodemask->maskp, returned_nodemask->size, address[i], MPOL_F_ADDR), 0);
        for(int i=0; i < numa_num_possible_nodes(); i++) {
            if(numa_bitmask_isbitset(returned_nodemask, i)) {
                EXPECT_TRUE(numa_bitmask_isbitset(expected_nodemask, i));
            }
        }
    }

    numa_free_nodemask(expected_nodemask);
    numa_free_nodemask(returned_nodemask);
}

void Check::check_node_hbw_interleave()
{
    int status = -1;
    struct bitmask *expected_nodemask = numa_allocate_nodemask(), *returned_nodemask = numa_allocate_nodemask();

    memkind_hbw_all_get_mbind_nodemask(NULL, expected_nodemask->maskp, expected_nodemask->size);
    //check if policy is MPOL_INTERLEAVE
    for (size_t i = 0; i < num_address; i++) {
        ASSERT_EQ(get_mempolicy(&status, returned_nodemask->maskp, returned_nodemask->size, address[i], MPOL_F_ADDR), 0);
        EXPECT_EQ(status, MPOL_INTERLEAVE);
        EXPECT_TRUE(numa_bitmask_equal(expected_nodemask, returned_nodemask));
    }

    numa_free_nodemask(expected_nodemask);
    numa_free_nodemask(returned_nodemask);
}

int Check::check_zero(void)
{
    size_t i;
    const char *cptr = (char *)ptr;
    for (i = 0; i < size; ++i) {
        if (cptr[i] != '\0') {
            return -1;
        }
    }
    return 0;
}

int Check::check_data(int data)
{
    int ret;
    void *p;
    p = malloc(size);
    memset(p, data, size);
    memset((void*)ptr, data, size);
    ret = memcmp(p, ptr, size);
    free(p);
    return ret;
}

int Check::check_align(size_t align)
{
    return (size_t)ptr % align;
}

string Check::skip_to_next_entry (ifstream &ip)
{
    string temp, token;
    size_t found = 0;
    string empty ="";

    while (!ip.eof()) {
        getline (ip, temp);
        found = temp.find("-");
        if (found != string::npos) {
            istringstream iss(temp);
            getline(iss, token, ' ');
            return token;
        }
    }
    return empty;
}

string Check::skip_to_next_kpage(ifstream &ip)
{
    string temp, token;
    size_t found = 0;
    string empty ="";

    while (!ip.eof()) {
        getline (ip, temp);
        found = temp.find("KernelPageSize:");
        if (found != string::npos) {
            return temp;
        }
    }
    return empty;
}


void Check::get_address_range(string &line,
                              unsigned long long *start_addr,
                              unsigned long long *end_addr)
{
    stringstream ss(line);
    string token;

    getline(ss, token, '-');
    *start_addr = strtoul(token.c_str(),
                          NULL,
                          16);
    getline(ss, token, '-');
    *end_addr = strtoul(token.c_str(),
                        NULL,
                        16);
}

size_t Check::get_kpagesize(string line)
{
    stringstream ss(line);
    string token;
    size_t pagesize;


    ss  >> token;
    ss  >> token;

    pagesize = atol(token.c_str());

    return (size_t)pagesize;
}

int Check::check_page_size(size_t page_size)
{
    int err = 0;
    size_t i;

    ip.open ("/proc/self/smaps");

    populate_smaps_table();
    if (check_page_size(page_size, address[0])) {
        err = -1;
    }
    for (i = 1; i < num_address && !err; ++i) {
        if (check_page_size(page_size, address[i])) {
            err = i;
        }
    }
    return err;
}

int Check::populate_smaps_table ()
{
    string read;
    size_t lpagesize;
    smaps_entry_t lentry;
    unsigned long long start_addr;
    unsigned long long end_addr;

    ip >> read;
    while (!ip.eof()) {

        start_addr = end_addr = 0;
        get_address_range (read,
                           &start_addr,
                           &end_addr);
        read = skip_to_next_kpage(ip);
        getline(ip, read);
        lpagesize = get_kpagesize(read);
        lpagesize *= 1024;
        lentry.start_addr = start_addr;
        lentry.end_addr = end_addr;
        lentry.pagesize = lpagesize;
        smaps_table.push_back(lentry);
        read = skip_to_next_entry(ip);
        if (read.empty()) {
            break;
        }
    }

    if (0 == smaps_table.size()) {
        fprintf(stderr,"Empty smaps table\n");
        return -1;
    }
    else {
        return 0;
    }

}

int Check::check_page_size(size_t page_size, void *vaddr)
{
    string read;
    unsigned long long virt_addr;
    size_t lpagesize;
    list<smaps_entry_t>::iterator it;
    unsigned long long start_addr;
    unsigned long long end_addr;

    virt_addr = (unsigned long long)(vaddr);

    for (it = smaps_table.begin();
         it != smaps_table.end();
         it++) {

        start_addr = it->start_addr;
        end_addr = it->end_addr;

        if ((virt_addr >= start_addr) &&
            (virt_addr < end_addr)) {
            lpagesize = it->pagesize;
            if (lpagesize == page_size) {
                return 0;
            }
            else {
                /*The pagesize of allocation and req don't match*/
                fprintf(stderr,"%zd does not match entry in SMAPS (%zd)\n",
                        page_size, lpagesize);
                return -1;
            }
        }
    }
    /*Never found a match!*/
    return 1;
}
