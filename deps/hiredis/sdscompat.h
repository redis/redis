/*
 * Copyright (c) 2020, Michael Grunder <michael dot grunder at gmail dot com>
 *
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

/*
 * SDS compatibility header.
 *
 * This simple file maps sds types and calls to their unique hiredis symbol names.
 * It's useful when we build Hiredis as a dependency of Redis and want to call
 * Hiredis' sds symbols rather than the ones built into Redis, as the libraries
 * have slightly diverged and could cause hard to track down ABI incompatibility
 * bugs.
 *
 */

#ifndef HIREDIS_SDS_COMPAT
#define HIREDIS_SDS_COMPAT

#define sds hisds

#define sdslen hi_sdslen
#define sdsavail hi_sdsavail
#define sdssetlen hi_sdssetlen
#define sdsinclen hi_sdsinclen
#define sdsalloc hi_sdsalloc
#define sdssetalloc hi_sdssetalloc

#define sdsAllocPtr hi_sdsAllocPtr
#define sdsAllocSize hi_sdsAllocSize
#define sdscat hi_sdscat
#define sdscatfmt hi_sdscatfmt
#define sdscatlen hi_sdscatlen
#define sdscatprintf hi_sdscatprintf
#define sdscatrepr hi_sdscatrepr
#define sdscatsds hi_sdscatsds
#define sdscatvprintf hi_sdscatvprintf
#define sdsclear hi_sdsclear
#define sdscmp hi_sdscmp
#define sdscpy hi_sdscpy
#define sdscpylen hi_sdscpylen
#define sdsdup hi_sdsdup
#define sdsempty hi_sdsempty
#define sds_free hi_sds_free
#define sdsfree hi_sdsfree
#define sdsfreesplitres hi_sdsfreesplitres
#define sdsfromlonglong hi_sdsfromlonglong
#define sdsgrowzero hi_sdsgrowzero
#define sdsIncrLen hi_sdsIncrLen
#define sdsjoin hi_sdsjoin
#define sdsjoinsds hi_sdsjoinsds
#define sdsll2str hi_sdsll2str
#define sdsMakeRoomFor hi_sdsMakeRoomFor
#define sds_malloc hi_sds_malloc
#define sdsmapchars hi_sdsmapchars
#define sdsnew hi_sdsnew
#define sdsnewlen hi_sdsnewlen
#define sdsrange hi_sdsrange
#define sds_realloc hi_sds_realloc
#define sdsRemoveFreeSpace hi_sdsRemoveFreeSpace
#define sdssplitargs hi_sdssplitargs
#define sdssplitlen hi_sdssplitlen
#define sdstolower hi_sdstolower
#define sdstoupper hi_sdstoupper
#define sdstrim hi_sdstrim
#define sdsull2str hi_sdsull2str
#define sdsupdatelen hi_sdsupdatelen

#endif /* HIREDIS_SDS_COMPAT */
