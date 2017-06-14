/*
 * Copyright (c) 2017, Andreas Bluemle <andreas dot bluemle at itxperts dot de>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef USE_NVML
#include "server.h"
#include "obj.h"
#include "libpmemobj.h"
#include "list.h"
#include "util.h"

int
pmemReconstruct(void)
{
    uint64_t i = 0;
    TOID(struct redis_pmem_root) root;
    TOID(struct dictEntryPM) entryPM_oid;
    dict *d;
    dictEntryPM *entryPM;
    /* void *key; */
    /* void *val; */
    void *pmem_base_addr;
    robjPM *objectPM;

    root = POBJ_ROOT(server.pm_pool, struct redis_pmem_root);
    pmem_base_addr = (void *)server.pm_pool->addr;
    serverLog(LL_NOTICE,"pmemReconstruct: %ld entries, base_addr 0x%lx", D_RO(root)->num_dict_entries, (uint64_t)pmem_base_addr);
    d = server.db[0].dict;
    dictExpand(d, D_RO(root)->num_dict_entries);
    POBJ_LIST_FOREACH(entryPM_oid, &D_RO(root)->head, pmem_list) {
        i++;
	/* entryPM = pmemobj_direct(entryPM_oid.oid); */
	entryPM = (dictEntryPM *)(entryPM_oid.oid.off + (uint64_t)pmem_base_addr);
	entryPM->key = (void *)(entryPM->key_oid.off + (uint64_t)pmem_base_addr);
	entryPM->v.val = (void *)(entryPM->val_oid.off + (uint64_t)pmem_base_addr);
        objectPM = entryPM->v.val;
        /* serverLog(LL_NOTICE,"pmemReconstruct: dictEntry %ld, off 0x%ld @ 0x%lx, key 0x%lx, val object 0x%lx", i, entryPM_oid.oid.off, (uint64_t)entryPM, (uint64_t)entryPM->key, (uint64_t)entryPM->v.val); */
        if (objectPM->type == OBJ_STRING) {
            /* serverLog(LL_NOTICE,"pmemReconstruct: redis object type %d encoding %d", objectPM->type, objectPM->encoding); */
            if (objectPM->encoding == OBJ_ENCODING_RAW || objectPM->encoding == OBJ_ENCODING_EMBSTR) {
                if (objectPM->encoding == OBJ_ENCODING_RAW) {
                    objectPM->ptr = (void *)(objectPM->ptr_oid.off + (uint64_t)pmem_base_addr);
                } else if (objectPM->encoding == OBJ_ENCODING_EMBSTR) {
                    struct sdshdr8 *sh;
                    sh = (void *)(objectPM+1);
                    objectPM->ptr = sh+1;
                }
                (void)dictAddReconstructedPM(d, (dictEntry *)entryPM);
                /* serverLog(LL_NOTICE,"pmemReconstruct: redis object ptr 0x%lx", (uint64_t)objectPM->ptr); */
            } else {
                serverLog(LL_WARNING,"pmemReconstruct: unexpected redis object encoding %d", objectPM->encoding);
            }
        } else {
            serverLog(LL_WARNING,"pmemReconstruct: unexpected redis object type %d", objectPM->type);
        }
    }
    return C_OK;
}

#endif
