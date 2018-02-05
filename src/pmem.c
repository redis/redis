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

#ifdef USE_PMDK
#include "server.h"
#include "obj.h"
#include "libpmemobj.h"
#include "util.h"

int
pmemReconstruct(void)
{
    TOID(struct redis_pmem_root) root;
    TOID(struct key_val_pair_PM) kv_PM_oid;
    struct key_val_pair_PM *kv_PM;
    dict *d;
    void *key;
    void *val;
    void *pmem_base_addr;

    root = server.pm_rootoid;
    pmem_base_addr = (void *)server.pm_pool->addr;
    d = server.db[0].dict;
    dictExpand(d, D_RO(root)->num_dict_entries);
    for (kv_PM_oid = D_RO(root)->pe_first; TOID_IS_NULL(kv_PM_oid) == 0; kv_PM_oid = D_RO(kv_PM_oid)->pmem_list_next){
		kv_PM = (key_val_pair_PM *)(kv_PM_oid.oid.off + (uint64_t)pmem_base_addr);
		key = (void *)(kv_PM->key_oid.off + (uint64_t)pmem_base_addr);
		val = (void *)(kv_PM->val_oid.off + (uint64_t)pmem_base_addr);

        (void)dictAddReconstructedPM(d, key, val);
    }
    return C_OK;
}

void pmemKVpairSet(void *key, void *val)
{
    PMEMoid *kv_PM_oid;
    PMEMoid val_oid;
    struct key_val_pair_PM *kv_PM_p;

    kv_PM_oid = sdsPMEMoidBackReference((sds)key);
    kv_PM_p = (struct key_val_pair_PM *)pmemobj_direct(*kv_PM_oid);

    val_oid.pool_uuid_lo = server.pool_uuid_lo;
    val_oid.off = (uint64_t)val - (uint64_t)server.pm_pool->addr;

    TX_ADD_FIELD_DIRECT(kv_PM_p, val_oid);
    kv_PM_p->val_oid = val_oid;
    return;
}

PMEMoid
pmemAddToPmemList(void *key, void *val)
{
    PMEMoid key_oid;
    PMEMoid val_oid;
    PMEMoid kv_PM;
    struct key_val_pair_PM *kv_PM_p;
    TOID(struct key_val_pair_PM) typed_kv_PM;
    struct redis_pmem_root *root;

    key_oid.pool_uuid_lo = server.pool_uuid_lo;
    key_oid.off = (uint64_t)key - (uint64_t)server.pm_pool->addr;

    val_oid.pool_uuid_lo = server.pool_uuid_lo;
    val_oid.off = (uint64_t)val - (uint64_t)server.pm_pool->addr;

    kv_PM = pmemobj_tx_zalloc(sizeof(struct key_val_pair_PM), pm_type_key_val_pair_PM);
    kv_PM_p = (struct key_val_pair_PM *)pmemobj_direct(kv_PM);
    kv_PM_p->key_oid = key_oid;
    kv_PM_p->val_oid = val_oid;
    typed_kv_PM.oid = kv_PM;

    root = pmemobj_direct(server.pm_rootoid.oid);

    kv_PM_p->pmem_list_next = root->pe_first;
    if(!TOID_IS_NULL(root->pe_first)) {
        struct key_val_pair_PM *head = D_RW(root->pe_first);
        TX_ADD_FIELD_DIRECT(head,pmem_list_prev);
    	head->pmem_list_prev = typed_kv_PM;
    }

    TX_ADD_DIRECT(root);
    root->pe_first = typed_kv_PM;
    root->num_dict_entries++;

    return kv_PM;
}

void
pmemRemoveFromPmemList(PMEMoid kv_PM_oid)
{
    TOID(struct key_val_pair_PM) typed_kv_PM;
    struct redis_pmem_root *root;

    root = pmemobj_direct(server.pm_rootoid.oid);

    typed_kv_PM.oid = kv_PM_oid;

    if(TOID_EQUALS(root->pe_first, typed_kv_PM)) {
    	TOID(struct key_val_pair_PM) typed_kv_PM_next = D_RO(typed_kv_PM)->pmem_list_next;
    	if(!TOID_IS_NULL(typed_kv_PM_next)){
    		struct key_val_pair_PM *next = D_RW(typed_kv_PM_next);
    		TX_ADD_FIELD_DIRECT(next,pmem_list_prev);
    		next->pmem_list_prev.oid = OID_NULL;
    	}
    	TX_FREE(root->pe_first);
    	TX_ADD_DIRECT(root);
    	root->pe_first = typed_kv_PM_next;
        root->num_dict_entries--;
        return;
    }
    else {
    	TOID(struct key_val_pair_PM) typed_kv_PM_prev = D_RO(typed_kv_PM)->pmem_list_prev;
    	TOID(struct key_val_pair_PM) typed_kv_PM_next = D_RO(typed_kv_PM)->pmem_list_next;
    	if(!TOID_IS_NULL(typed_kv_PM_prev)){
    		struct key_val_pair_PM *prev = D_RW(typed_kv_PM_prev);
    		TX_ADD_FIELD_DIRECT(prev,pmem_list_next);
    		prev->pmem_list_next = typed_kv_PM_next;
    	}
    	if(!TOID_IS_NULL(typed_kv_PM_next)){
    		struct key_val_pair_PM *next = D_RW(typed_kv_PM_next);
    		TX_ADD_FIELD_DIRECT(next,pmem_list_prev);
    		next->pmem_list_prev = typed_kv_PM_prev;
    	}
    	TX_FREE(typed_kv_PM);
    	TX_ADD_FIELD_DIRECT(root,num_dict_entries);
        root->num_dict_entries--;
        return;
    }
}
#endif
