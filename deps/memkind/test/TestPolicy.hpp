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
#include <memkind/internal/memkind_hbw.h>

#include "common.h"
#include "allocator_perf_tool/GTestAdapter.hpp"
#include "numa.h"

#include <memory>

namespace TestPolicy
{
    typedef std::unique_ptr<struct bitmask, decltype(&numa_free_nodemask)> unique_bitmask_ptr;

    unique_bitmask_ptr make_nodemask_ptr()
    {
        return std::move(unique_bitmask_ptr(numa_allocate_nodemask(), numa_free_nodemask));
    }

    unique_bitmask_ptr make_cpumask_ptr()
    {
        return std::move(unique_bitmask_ptr(numa_allocate_cpumask(), numa_free_nodemask));
    }

    int get_num_of_pages(const size_t size, const size_t page_size)
    {
        size_t pages_number = size / page_size;
        pages_number += size % page_size ? 1 : 0;
        return pages_number;
    }

    std::vector<void*> get_address_of_pages(const void* ptr, const size_t pages_number, const size_t page_size)
    {
        std::vector<void*> address(pages_number);
        const size_t page_mask = ~(page_size-1);
        address[0] = (void*)((uintptr_t)ptr & page_mask); //aligned address of first page
        for (size_t page_num = 1; page_num < pages_number; page_num++) {
            address[page_num] = (char*)address[page_num-1] + page_size;
        }
        return address;
    }

    void record_page_association(const void *ptr, const size_t size, const size_t page_size)
    {
        size_t pages_number = get_num_of_pages(size, page_size);
        std::vector<void*> address = get_address_of_pages(ptr, pages_number, page_size);

        int max_node_id = numa_max_node();
        std::vector<int> nodes(pages_number);
        std::vector<int> pages_on_node(max_node_id+1);

        if (move_pages(0, pages_number, address.data(), NULL, nodes.data(), MPOL_MF_MOVE)) {
            fprintf(stderr, "Error: move_pages() returned %s\n", strerror(errno));
            return;
        }

        for (size_t i = 0; i < pages_number; i++) {
            if (nodes[i] < 0) {
                fprintf(stderr,"Error: status of page %p is %d\n", address[i], nodes[i]);
                return;
            } else {
                pages_on_node[nodes[i]]++;
            }
        }

        for (size_t i = 0; i < (size_t)max_node_id + 1; i++) {
            if (pages_on_node[i] > 0) {
                char buffer[1024];
                snprintf(buffer, sizeof(buffer), "Node%zd", i);
                GTestAdapter::RecordProperty(buffer, pages_on_node[i]);
            }
        }
    }

    void check_numa_nodes(unique_bitmask_ptr& expected_bitmask, int policy, void* ptr, size_t size)
    {

        const size_t page_size = sysconf(_SC_PAGESIZE);
        size_t pages_number = get_num_of_pages(size, page_size);
        std::vector<void*> address = get_address_of_pages(ptr, pages_number, page_size);
        unique_bitmask_ptr returned_bitmask = make_nodemask_ptr();
        int status = -1;

        for (size_t page_num = 0; page_num < address.size(); page_num++) {
            ASSERT_EQ(0, get_mempolicy(&status, returned_bitmask->maskp, returned_bitmask->size, address[page_num], MPOL_F_ADDR));
            ASSERT_EQ(policy, status);
            switch(policy) {
                case MPOL_INTERLEAVE:
                    EXPECT_TRUE(numa_bitmask_equal(expected_bitmask.get(), returned_bitmask.get()));
                    break;
                case MPOL_DEFAULT:
                    break;
                case MPOL_BIND:
                case MPOL_PREFERRED:
                    for(int i=0; i < numa_num_possible_nodes(); i++) {
                        if(numa_bitmask_isbitset(returned_bitmask.get(), i)) {
                            EXPECT_TRUE(numa_bitmask_isbitset(expected_bitmask.get(), i));
                        }
                    }
                    break;
                default:
                    assert(!"Unknown policy\n");
            }
        }
    }

    void check_hbw_numa_nodes(int policy, void* ptr, size_t size)
    {
        unique_bitmask_ptr expected_bitmask = make_nodemask_ptr();

        memkind_hbw_all_get_mbind_nodemask(NULL, expected_bitmask->maskp, expected_bitmask->size);
        check_numa_nodes(expected_bitmask, policy, ptr, size);
    }

    void check_all_numa_nodes(int policy, void* ptr, size_t size)
    {
        if (policy != MPOL_INTERLEAVE && policy != MPOL_DEFAULT) return;

        unique_bitmask_ptr expected_bitmask = make_nodemask_ptr();

        for(int i=0; i < numa_num_configured_nodes(); i++) {
            numa_bitmask_setbit(expected_bitmask.get(), i);
        }

        check_numa_nodes(expected_bitmask, policy, ptr, size);
    }
}
