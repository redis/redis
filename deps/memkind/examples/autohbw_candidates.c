/*
 * Copyright (C) 2015 - 2016 Intel Corporation.
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

///////////////////////////////////////////////////////////////////////////
// File   : autohbw_candidates.c
// Purpose: Shows which functions are interposed by AutoHBW library.
//        : These functions can be used for testing purposes
// Author : Ruchira Sasanka (ruchira.sasanka AT intel.com)
// Date   : Sept 10, 2015
///////////////////////////////////////////////////////////////////////////


#include <memkind.h>

#include <stdlib.h>
#include <stdio.h>


///////////////////////////////////////////////////////////////////////////
// This function contains an example case for each heap allocation function
// intercepted by the AutoHBW library
///////////////////////////////////////////////////////////////////////////

//volatile is needed to prevent optimizing out below hooks
volatile int memkind_called_g;

void memkind_malloc_post(struct memkind *kind, size_t size, void **result)
{
  memkind_called_g = 1;
}
void memkind_calloc_post(struct memkind *kind, size_t nmemb, size_t size, void **result)
{
  memkind_called_g = 1;
}
void memkind_posix_memalign_post(struct memkind *kind, void **memptr, size_t alignment, size_t size, int *err)
{
  memkind_called_g = 1;
}
void memkind_realloc_post(struct memkind *kind, void *ptr, size_t size, void **result)
{
  memkind_called_g = 1;
}
void memkind_free_pre(struct memkind **kind, void **ptr)
{
  memkind_called_g = 1;
}

void finish_testcase(int fail_condition, const char* fail_message, int *err)
{

  if(memkind_called_g != 1 || fail_condition)
  {
    printf("%s\n", fail_message);
    *err= -1;
  }
  memkind_called_g = 0;
}

int main() 
{
  int err = 0;
  const size_t size = 1024 * 1024;   // 1M of data

  void *buf = NULL;
  memkind_called_g = 0;

  // Test 1: Test malloc and free
  buf = malloc(size);
  finish_testcase(buf==NULL, "Malloc failed!", &err);

  free(buf);
  finish_testcase(0, "Free after malloc failed!", &err);

  // Test 2: Test calloc and free
  buf = calloc(size, 1);
  finish_testcase(buf==NULL, "Calloc failed!", &err);

  free(buf);
  finish_testcase(0, "Free after calloc failed!", &err);

  // Test 3: Test realloc and free
  buf = malloc(size);
  finish_testcase(buf==NULL, "Malloc before realloc failed!", &err);

  buf = realloc(buf,  size * 2);
  finish_testcase(buf==NULL, "Realloc failed!", &err);

  free(buf);
  finish_testcase(0, "Free after realloc failed!", &err);

  // Test 4: Test posix_memalign and free
  int ret = posix_memalign(&buf,  64, size);
  finish_testcase(ret, "Posix_memalign failed!", &err);

  free(buf);
  finish_testcase(0, "Free after posix_memalign failed!", &err);

  return err;
}
