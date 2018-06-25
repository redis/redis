/*
    Copyright (c) 2005-2017 Intel Corporation

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.




*/

#include <stdint.h>

typedef void *(*rawAllocType)(intptr_t pool_id, size_t* bytes);
typedef int   (*rawFreeType)(intptr_t pool_id, void* raw_ptr, size_t raw_bytes);

struct MemPoolPolicy {
    rawAllocType pAlloc;
    rawFreeType  pFree;
    size_t       granularity;
    int          version;
    unsigned     fixedPool : 1,
                 keepAllMemory : 1,
                 reserved : 30;
};