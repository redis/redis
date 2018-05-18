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

#include <memkind.h>

#include <errno.h>
#include <gtest/gtest.h>

/* Tests which calls APIS in wrong ways to generate Error Messages thrown by the
 * the memkind library
 */

const int all_error_code[] = {
    MEMKIND_ERROR_UNAVAILABLE,
    MEMKIND_ERROR_MBIND,
    MEMKIND_ERROR_MMAP,
    MEMKIND_ERROR_MALLOC,
    MEMKIND_ERROR_RUNTIME,
    MEMKIND_ERROR_ENVIRON,
    MEMKIND_ERROR_INVALID,
    MEMKIND_ERROR_TOOMANY,
    MEMKIND_ERROR_BADOPS,
    MEMKIND_ERROR_HUGETLB,
    EINVAL,
    ENOMEM
};

class ErrorMessage: public :: testing :: Test
{
protected:

    void SetUp()
    {
    }
    void TearDown()
    {
    }
};

TEST_F(ErrorMessage, test_TC_MEMKIND_ErrorMsgLength)
{
    size_t i;
    char error_message[MEMKIND_ERROR_MESSAGE_SIZE];
    for (i = 0; i < sizeof(all_error_code)/sizeof(all_error_code[0]); ++i) {
        memkind_error_message(all_error_code[i], error_message, MEMKIND_ERROR_MESSAGE_SIZE);
        EXPECT_TRUE(strlen(error_message) < MEMKIND_ERROR_MESSAGE_SIZE - 1);
    }
    memkind_error_message(MEMKIND_ERROR_UNAVAILABLE, NULL, 0);
}

TEST_F(ErrorMessage, test_TC_MEMKIND_ErrorMsgFormat)
{
    size_t i;
    char error_message[MEMKIND_ERROR_MESSAGE_SIZE];
    for (i = 0; i < sizeof(all_error_code)/sizeof(all_error_code[0]); ++i) {
        memkind_error_message(all_error_code[i], error_message, MEMKIND_ERROR_MESSAGE_SIZE);
        EXPECT_TRUE(strncmp(error_message, "<memkind>", strlen("<memkind>")) == 0);
    }
}

TEST_F(ErrorMessage, test_TC_MEMKIND_ErrorMsgUndefMesg)
{
    char error_message[MEMKIND_ERROR_MESSAGE_SIZE];
    memkind_error_message(-0xdeadbeef, error_message, MEMKIND_ERROR_MESSAGE_SIZE);
    EXPECT_TRUE(strncmp(error_message, "<memkind> Undefined error number:", strlen("<memkind> Undefined error number:")) == 0);
}

