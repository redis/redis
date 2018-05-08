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
#include "allocator_perf_tool/HugePageOrganizer.hpp"

#include "trial_generator.h"
#include "check.h"
#include <vector>
#include <numa.h>

void TrialGenerator :: generate_incremental(alloc_api_t api)
{

    size_t size[] = {2, 2*KB, 2*MB};
    size_t psize[] = {4096, 4096, 2097152};
    size_t align[] = {8, 128, 4*KB};
    int k = 0;
    trial_vec.clear();
    for (int i = 0; i< (int)(sizeof(size)/sizeof(size[0]));
         i++) {
        trial_vec.push_back(create_trial_tuple(api, size[i],
                                               align[i], psize[i],
                                               MEMKIND_HBW,-1));
        if (i > 0)
            k++;
        trial_vec.push_back(create_trial_tuple(HBW_FREE,0,0,0,
                                               MEMKIND_HBW, k++));

        trial_vec.push_back(create_trial_tuple(api, size[i],
                                               align[i], psize[i],
                                               MEMKIND_HBW_PREFERRED,-1));
        k++;
        trial_vec.push_back(create_trial_tuple(HBW_FREE,0,0,0,
                                               MEMKIND_HBW_PREFERRED, k++));
    }
}


void TrialGenerator :: generate_recycle_incremental(alloc_api_t api)
{

    size_t size[] = {2*MB, 2*GB};
    int k = 0;
    trial_vec.clear();
    for (int i = 0; i < (int)(sizeof(size)/sizeof(size[0]));
         i++) {
        trial_vec.push_back(create_trial_tuple(api, size[i], 0, 0,
                                               MEMKIND_DEFAULT,-1));
        if (i > 0)
            k++;
        trial_vec.push_back(create_trial_tuple(MEMKIND_FREE,0,0,0,
                                               MEMKIND_DEFAULT, k++));

        trial_vec.push_back(create_trial_tuple(api, size[i], 0, 0,
                                               MEMKIND_HBW,-1));
        k++;
        trial_vec.push_back(create_trial_tuple(MEMKIND_FREE,0,0,0,
                                               MEMKIND_HBW, k++));

        trial_vec.push_back(create_trial_tuple(api, size[i], 0, 0,
                                               MEMKIND_HBW_PREFERRED,-1));
        k++;
        trial_vec.push_back(create_trial_tuple(MEMKIND_FREE,0,0,0,
                                               MEMKIND_HBW_PREFERRED, k++));

    }

}

trial_t TrialGenerator :: create_trial_tuple(alloc_api_t api,
        size_t size,
        size_t alignment,
        int page_size,
        memkind_t memkind,
        int free_index)
{
    trial_t ltrial;
    ltrial.api = api;
    ltrial.size = size;
    ltrial.alignment = alignment;
    ltrial.page_size = page_size;
    ltrial.memkind = memkind;
    ltrial.free_index = free_index;
    return ltrial;
}


void TrialGenerator :: generate_gb (alloc_api_t api, int number_of_gb_pages, memkind_t memkind, alloc_api_t api_free, bool psize_strict, size_t align)
{
    ASSERT_GBPAGES_AVAILABILITY();
    std::vector<size_t> sizes_to_alloc;
    //When API = HBW_MEMALIGN_PSIZE: psize is set to HBW_PAGESIZE_1GB_STRICT when allocation is a multiple of 1GB. Otherwise it is set to HBW_PAGESIZE_1GB.
    for (int i=1; i <= number_of_gb_pages; i++) {
            if (psize_strict || api!=HBW_MEMALIGN_PSIZE)
                sizes_to_alloc.push_back(i*GB);
            else
                sizes_to_alloc.push_back(i*GB+1);
    }
    int k = 0;
    trial_vec.clear();

    for (int i = 0; i< (int)sizes_to_alloc.size(); i++)
    {
        trial_vec.push_back(create_trial_tuple(api, sizes_to_alloc[i],
                                               align, GB,
                                               memkind,
                                               -1));
        if (i > 0)
            k++;
        trial_vec.push_back(create_trial_tuple(api_free,0,0,0,
                                               memkind,
                                               k++));
    }
}

int n_random(int i)
{
    return random() % i;
}

void TrialGenerator :: generate_recycle_psize_2GB(alloc_api_t api)
{
    ASSERT_HUGEPAGES_AVAILABILITY();
    trial_vec.clear();
    trial_vec.push_back(create_trial_tuple(api, 2*GB, 32, 4096,
                                           MEMKIND_HBW,-1));
    trial_vec.push_back(create_trial_tuple(MEMKIND_FREE, 0, 0, 0,
                                           MEMKIND_HBW, 0));
    trial_vec.push_back(create_trial_tuple(api, 2*GB, 32, 2097152,
                                           MEMKIND_HBW_HUGETLB,-1));
    trial_vec.push_back(create_trial_tuple(MEMKIND_FREE, 0, 0, 2097152,
                                           MEMKIND_HBW_HUGETLB, 2));

}

void TrialGenerator :: generate_recycle_psize_incremental(alloc_api_t api)
{
    ASSERT_HUGEPAGES_AVAILABILITY();
    size_t size[] = {2*KB, 2*MB};

    int k = 0;
    trial_vec.clear();
    for (int i = 0; i < (int)(sizeof(size)/sizeof(size[0]));
         i++) {
        trial_vec.push_back(create_trial_tuple(api, size[i], 32, 4096,
                                               MEMKIND_HBW,-1));
        if (i > 0)
            k++;
        trial_vec.push_back(create_trial_tuple(MEMKIND_FREE, 0, 0, 0,
                                               MEMKIND_HBW, k++));

        trial_vec.push_back(create_trial_tuple(api, size[i], 32, 2097152,
                                               MEMKIND_HBW_PREFERRED_HUGETLB,-1));
        k++;
        trial_vec.push_back(create_trial_tuple(MEMKIND_FREE, 0, 0, 0,
                                               MEMKIND_HBW_PREFERRED_HUGETLB, k++));
    }
}



void TrialGenerator :: generate_size_1KB_2GB(alloc_api_t api)
{

    size_t size[] = {KB, 2*KB, 4*KB, 16*KB, 256*KB,
                     512*KB, MB, 2*MB, 4*MB, 16*MB,
                     256*MB, 512*MB, GB, 2*GB
                    };

    int k = 0;
    trial_vec.clear();
    for (unsigned int i = 0; i < (int)(sizeof(size)/sizeof(size[0]));
         i++) {
        trial_vec.push_back(create_trial_tuple(api,size[i],32,
                                               4096, MEMKIND_HBW,
                                               -1));
        if (i > 0)
            k++;
        trial_vec.push_back(create_trial_tuple(HBW_FREE, 0, 0, 0,
                                               MEMKIND_HBW, k));
        k++;
    }
}

void TrialGenerator :: generate_interleave(alloc_api_t api)
{
    size_t size[] = {4*KB, 2*MB, 2*GB};
    size_t psize = 4096;
    int k = 0;
    trial_vec.clear();
    for (size_t i = 0; i < (sizeof(size)/sizeof(size[0])); i++) {
        trial_vec.push_back(create_trial_tuple(api, size[i],0, psize,
                                               MEMKIND_HBW_INTERLEAVE,-1));

        if (i > 0)
            k++;
        trial_vec.push_back(create_trial_tuple(HBW_FREE,0,0,0,
                                               MEMKIND_HBW_INTERLEAVE, k++));
    }
}

void TrialGenerator :: generate_size_2bytes_2KB_2MB(alloc_api_t api)
{
    size_t size[] = {2, 2*KB, 2*MB};

    int k = 0;
    trial_vec.clear();
    for (unsigned int i = 0; i < (int)(sizeof(size)/sizeof(size[0]));
         i++) {
        trial_vec.push_back(
            create_trial_tuple(
                api,size[i],
                32,
                4096,
                MEMKIND_HBW,
                -1
            )
        );

        if (i > 0) k++;
        trial_vec.push_back(create_trial_tuple(HBW_FREE, 0, 0, 0,
                                               MEMKIND_HBW, k));
        k++;
    }
}

void TrialGenerator :: print()
{

    std::vector<trial_t>:: iterator it;

    std::cout <<"*********** Size: "<< trial_vec.size()
              <<"********\n";
    std::cout << "SIZE PSIZE ALIGN FREE KIND"<<std::endl;

    for (it = trial_vec.begin();
         it != trial_vec.end();
         it++) {
        std::cout << it->size <<" "
                  << it->page_size <<" "
                  << it->alignment <<" "
                  << it->free_index <<" "
                  << it->memkind <<" "
                  <<std::endl;
    }

}


void TrialGenerator :: run(int num_bandwidth, std::vector<int> &bandwidth)
{

    int num_trial = trial_vec.size();
    int i, ret = 0;
    void **ptr_vec = NULL;

    ptr_vec = (void **) malloc (num_trial *
                                sizeof (void *));
    if (NULL == ptr_vec) {
        fprintf (stderr, "Error in allocating ptr array\n");
        exit(-1);
    }

    for (i = 0; i < num_trial; ++i) {
        ptr_vec[i] = NULL;
    }
    for (i = 0; i < num_trial; ++i) {
        switch(trial_vec[i].api) {
            case HBW_FREE:
                if (i == num_trial - 1 || trial_vec[i + 1].api != HBW_REALLOC) {
                    hbw_free(ptr_vec[trial_vec[i].free_index]);
                    ptr_vec[trial_vec[i].free_index] = NULL;
                    ptr_vec[i] = NULL;
                }
                else {
                    ptr_vec[i + 1] = hbw_realloc(ptr_vec[trial_vec[i].free_index], trial_vec[i + 1].size);
                    ptr_vec[trial_vec[i].free_index] = NULL;
                }
                break;
            case MEMKIND_FREE:
                if (i == num_trial - 1 || trial_vec[i + 1].api != MEMKIND_REALLOC) {
                    memkind_free(trial_vec[i].memkind,
                                 ptr_vec[trial_vec[i].free_index]);
                    ptr_vec[trial_vec[i].free_index] = NULL;
                    ptr_vec[i] = NULL;
                }
                else {
                    ptr_vec[i + 1] = memkind_realloc(trial_vec[i].memkind,
                                                     ptr_vec[trial_vec[i].free_index],
                                                     trial_vec[i + 1].size);
                    ptr_vec[trial_vec[i].free_index] = NULL;
                }
                break;
            case HBW_MALLOC:
                fprintf (stdout,"Allocating %zd bytes using hbw_malloc\n",
                         trial_vec[i].size);
                ptr_vec[i] = hbw_malloc(trial_vec[i].size);
                break;
            case HBW_CALLOC:
                fprintf (stdout,"Allocating %zd bytes using hbw_calloc\n",
                         trial_vec[i].size);
                ptr_vec[i] = hbw_calloc(trial_vec[i].size, 1);
                break;
            case HBW_REALLOC:
                fprintf (stdout,"Allocating %zd bytes using hbw_realloc\n",
                         trial_vec[i].size);
                fflush(stdout);
                if (NULL == ptr_vec[i]) {
                    ptr_vec[i] = hbw_realloc(NULL, trial_vec[i].size);
                }
                break;
            case HBW_MEMALIGN:
                fprintf (stdout,"Allocating %zd bytes using hbw_memalign\n",
                         trial_vec[i].size);
                ret =  hbw_posix_memalign(&ptr_vec[i],
                                          trial_vec[i].alignment,
                                          trial_vec[i].size);
                break;
            case HBW_MEMALIGN_PSIZE:
                fprintf (stdout,"Allocating %zd bytes using hbw_memalign_psize\n",
                         trial_vec[i].size);
                hbw_pagesize_t psize;
                if (trial_vec[i].page_size == 4096)
                    psize = HBW_PAGESIZE_4KB;
                else if (trial_vec[i].page_size == 2097152)
                    psize = HBW_PAGESIZE_2MB;
                else if (trial_vec[i].size %
                         trial_vec[i].page_size > 0)
                    psize = HBW_PAGESIZE_1GB;
                else
                    psize = HBW_PAGESIZE_1GB_STRICT;

                ret = hbw_posix_memalign_psize(&ptr_vec[i],
                                               trial_vec[i].alignment,
                                               trial_vec[i].size,
                                               psize);

                break;
            case MEMKIND_MALLOC:
                fprintf (stdout,"Allocating %zd bytes using memkind_malloc\n",
                         trial_vec[i].size);
                ptr_vec[i] = memkind_malloc(trial_vec[i].memkind,
                                            trial_vec[i].size);
                break;
            case MEMKIND_CALLOC:
                fprintf (stdout,"Allocating %zd bytes using memkind_calloc\n",
                         trial_vec[i].size);
                ptr_vec[i] = memkind_calloc(trial_vec[i].memkind,
                                            trial_vec[i].size, 1);
                break;
            case MEMKIND_REALLOC:
                fprintf (stdout,"Allocating %zd bytes using memkind_realloc\n",
                         trial_vec[i].size);
                if (NULL == ptr_vec[i]) {
                    ptr_vec[i] = memkind_realloc(trial_vec[i].memkind,
                                                 ptr_vec[i],
                                                 trial_vec[i].size);
                }
                break;
            case MEMKIND_POSIX_MEMALIGN:
                fprintf (stdout,
                         "Allocating %zd bytes using memkind_posix_memalign\n",
                         trial_vec[i].size);

                ret = memkind_posix_memalign(trial_vec[i].memkind,
                                             &ptr_vec[i],
                                             trial_vec[i].alignment,
                                             trial_vec[i].size);
                break;
        }
        if (trial_vec[i].api != HBW_FREE &&
            trial_vec[i].api != MEMKIND_FREE &&
            trial_vec[i].memkind != MEMKIND_DEFAULT) {
            ASSERT_TRUE(ptr_vec[i] != NULL);
            memset(ptr_vec[i], 0, trial_vec[i].size);
            Check check(ptr_vec[i], trial_vec[i]);
            if (trial_vec[i].memkind != MEMKIND_DEFAULT &&
                trial_vec[i].memkind != MEMKIND_HUGETLB &&
                trial_vec[i].memkind != MEMKIND_GBTLB) {
                if (trial_vec[i].memkind == MEMKIND_HBW_INTERLEAVE) {
                    check.check_node_hbw_interleave();
                    EXPECT_EQ(0, check.check_page_size(trial_vec[i].page_size));
                }
                else {
                    check.check_node_hbw();
                }
            }
            if (trial_vec[i].api == HBW_CALLOC) {
                EXPECT_EQ(0, check.check_zero());
            }
            if (trial_vec[i].api == HBW_MEMALIGN ||
                trial_vec[i].api == HBW_MEMALIGN_PSIZE ||
                trial_vec[i].api == MEMKIND_POSIX_MEMALIGN) {
                EXPECT_EQ(0, check.check_align(trial_vec[i].alignment));
                EXPECT_EQ(0, ret);
            }
            if (trial_vec[i].api == HBW_MEMALIGN_PSIZE ||
                (trial_vec[i].api == MEMKIND_MALLOC &&
                 (trial_vec[i].memkind == MEMKIND_HBW_HUGETLB ||
                  trial_vec[i].memkind == MEMKIND_HBW_PREFERRED_HUGETLB))) {
                EXPECT_EQ(0, check.check_page_size(trial_vec[i].page_size));
            }
        }
    }
    for (i = 0; i < num_trial; ++i) {
        if (ptr_vec[i]) {
            hbw_free(ptr_vec[i]);
        }
    }
}

void TGTest :: SetUp()
{
    size_t node;
    char *hbw_nodes_env, *endptr;
    tgen = std::move(std::unique_ptr<TrialGenerator>(new TrialGenerator()));

    hbw_nodes_env = getenv("MEMKIND_HBW_NODES");
    if (hbw_nodes_env) {
        num_bandwidth = 128;
        for (node = 0; node < num_bandwidth; node++) {
            bandwidth.push_back(1);
        }
        node = strtol(hbw_nodes_env, &endptr, 10);
        bandwidth.push_back(2);
        while (*endptr == ':') {
            hbw_nodes_env = endptr + 1;
            node = strtol(hbw_nodes_env, &endptr, 10);
            if (endptr != hbw_nodes_env && node >= 0 && node < num_bandwidth) {
                bandwidth.push_back(2);
            }
        }
    }
    else {
        num_bandwidth = NUMA_NUM_NODES;
        nodemask_t nodemask;
        struct bitmask nodemask_bm = {NUMA_NUM_NODES, nodemask.n};
        numa_bitmask_clearall(&nodemask_bm);

        memkind_hbw_all_get_mbind_nodemask(NULL, nodemask.n, NUMA_NUM_NODES);

        int i, nodes_num = numa_num_configured_nodes();
        for (i=0; i<NUMA_NUM_NODES; i++) {
            if (i >= nodes_num) {
                bandwidth.push_back(0);
            }
            else if (numa_bitmask_isbitset(&nodemask_bm, i)) {
                bandwidth.push_back(2);
            }
            else {
                bandwidth.push_back(1);
            }
        }
    }
}

void TGTest :: TearDown()
{}
