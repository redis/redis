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

#include "memkind.h"
#include "memkind/internal/memkind_private.h"

#include "common.h"
#include "static_kinds_list.h"

/*
 * Set of tests for checking if static kinds meets non-trivial assumptions
 */

class StaticKindsTest: public :: testing::Test
{
public:

protected:
    void SetUp()
    {
    }
};


/*
 * Assumption: all static kinds should implement init_once operation
 * Reason:  init_once should perform memkind_register (and other initialization if needed)
 *          we are also using that fact to optimize initialization on first use (in memkind_malloc etc.)
 */
TEST_F(StaticKindsTest, test_TC_MEMKIND_STATIC_KINDS_INIT_ONCE)
{
    for(size_t i=0; i<(sizeof(static_kinds_list)/sizeof(static_kinds_list[0])); i++) {
       ASSERT_TRUE(static_kinds_list[i]->ops->init_once != NULL) << static_kinds_list[i]->name << " does not implement init_once operation!";
    }
}

