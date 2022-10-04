/* Copyright (c) 2021, ctrip.com
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

#include "ctrip_swap.h"

/* List meta */

#define SEGMENT_TYPE_HOT 0
#define SEGMENT_TYPE_COLD 1
#define SEGMENT_TYPE_BOTH 2

typedef struct segment {
  unsigned int type:1;
  unsigned int reserved:1;
  long index:62;
  long len;
} segment;

#define LIST_META_CAPCITY_DEFAULT 4
#define LIST_META_CAPCITY_LINEAR  1024
#define LIST_META_ITER_FINISHED LONG_MAX

typedef struct listMeta {
    long len; /* total len of segments */
    long num; /* num of segments */
    segment *segments; /* segments of rockslist or memlist */
    long capacity; /* capacity of segments */
} listMeta;

typedef struct listMetaIterator {
    long ridx; /* current rocks index */
    int segidx; /* current segment index */
    int segtype; /* current segment type */
    int segtypes; /* segment type type, SEGMENT_TYPE_BOTH iterates all ridx. */
    listMeta *meta; /* ref to meta */
} listMetaIterator;

static inline int segmentTypeMatch(int segtypes, int segtype) {
    return segtype == segtypes || segtypes == SEGMENT_TYPE_BOTH;
}

void listMetaIteratorInitWithType(listMetaIterator *iter, listMeta *meta, int segtypes) {
    int segidx = 0;
    segment *seg;

    iter->meta = meta;
    iter->segtypes = segtypes;

    if (meta->len <= 0) {
        iter->ridx = LIST_META_ITER_FINISHED;
        return;
    }
    
    serverAssert(meta->num > 0);

    /* skip leading empty segments or unma */
    while (segidx < meta->num) {
        seg = meta->segments + segidx;
        if (seg->len > 0 && segmentTypeMatch(segtypes,seg->type)) {
            iter->segidx = segidx;
            iter->ridx = seg->index;
            iter->segtype = seg->type;
            break;
        } else {
            segidx++;
        }
    }
}

void listMetaIteratorInit(listMetaIterator *iter, listMeta *meta) {
    listMetaIteratorInitWithType(iter,meta,SEGMENT_TYPE_BOTH);
}

void listMetaIteratorDeinit(listMetaIterator *iter) {
    UNUSED(iter);
}

int listMetaIterFinished(listMetaIterator *iter) {
    return iter->ridx == LIST_META_ITER_FINISHED;
}

void listMetaIterNext(listMetaIterator *iter) {
    int segidx = iter->segidx;
    segment *seg = NULL;

    if (listMetaIterFinished(iter)) return;

    iter->ridx++;

    /* current segment is ok */
    seg = iter->meta->segments + segidx;
    if (iter->ridx < seg->index + seg->len) return;

    /* current segment iter finished, find next valid segment. */
    while (1) {
        segidx++;

        /* can't find any valid segment. */
        if (segidx >= iter->meta->num) {
            iter->ridx = LIST_META_ITER_FINISHED;
            break;
        }

        seg = iter->meta->segments + segidx;

        /* segment candidate empty, try next segment. */
        if (seg->len == 0) continue;

        /* segment candidate type not match, try next segment. */
        if (!segmentTypeMatch(iter->segtypes,seg->type)) continue;

        /* candidate is confirmed valid by now. */
        iter->segidx = segidx;
        iter->ridx = seg->index;
        iter->segtype = seg->type;
        break;
    }
}

long listMetaIterCur(listMetaIterator *iter, int *segtype) {
    if (segtype) *segtype = iter->segtype;
    return iter->ridx;
}

int sortSegmentByIndex(const void *l_, const void *r_) {
    const segment *l = l_, *r = r_;
    return l->index - r->index;
}

listMeta *listMetaCreate() {
    listMeta *list_meta = zmalloc(sizeof(listMeta));
    list_meta->capacity = LIST_META_CAPCITY_DEFAULT;
    list_meta->len = 0;
    list_meta->num = 0;
    list_meta->segments = zmalloc(list_meta->capacity * sizeof(segment));
    return list_meta;
}

void listMetaFree(listMeta *list_meta) {
    if (list_meta == NULL) return;
    if (list_meta->segments) {
        zfree(list_meta->segments);
        list_meta->segments = NULL;
    }
    zfree(list_meta);
}

listMeta *listMetaNormalizeFromRequest(int num, range *ranges,
        long llen) {
    range *r;
    long req_len = 0;
    listMeta *req_meta = NULL;
    segment *seg, *segments = zmalloc(sizeof(segment)*num);

    for (int i = 0; i < num; i++) {
        r = ranges + i, seg = segments + i;

        /* See addListRangeReply for details. */
        if (r->start < 0) r->start += llen;
        if (r->end < 0) r->end += llen;
        if (r->start < 0) r->start = 0;

        if (r->start > r->end  || r->start > llen) goto end;
        if (r->end >= llen) r->end = llen-1;

        seg->type = SEGMENT_TYPE_HOT;
        seg->reserved = 0;
        seg->index = r->start;
        seg->len = r->end-r->start+1;

        req_len += seg->len;
    }

    req_meta = zmalloc(sizeof(listMeta));
    req_meta->len = req_len;
    req_meta->capacity = num;
    req_meta->num = num;
    req_meta->segments = segments;
    segments = NULL;

end:
    zfree(segments);
    return req_meta;
}

/* List meta segments are constinuous, req meta aren't . */ 
int listMetaIsValid(listMeta *list_meta, int strict) {
    segment *seg;
    long i, expected_len = 0, next_index = -1;

    for (i = 0; i < list_meta->num; i++) {
        seg = list_meta->segments + i;
        expected_len += seg->len;
        if (seg->len <= 0) {
            return 0;
        } else if (next_index == -1 || 
                (strict && next_index == seg->index) ||
                next_index <= seg->index) {
            next_index = seg->index + seg->len;
        } else {
            return 0;
        }
    }

    return expected_len == list_meta->len;
}

int listMetaEmpty(listMeta *list_meta) {
    return list_meta->len == 0;
}

static void listMetaMakeRoomFor(listMeta *list_meta, int num) {
    if (num <= list_meta->capacity) return;

    while (num > list_meta->capacity) {
        if (list_meta->capacity >= LIST_META_CAPCITY_LINEAR) {
            list_meta->capacity = list_meta->capacity+LIST_META_CAPCITY_LINEAR;
        } else {
            list_meta->capacity = list_meta->capacity*2;
        }
    }
    list_meta->segments = zrealloc(list_meta->segments,
            list_meta->capacity*sizeof(segment));
}

static int listMetaAppendSegment_(listMeta *list_meta, int type, long index,
        long len, int check) {
    segment *last, *cur;

    if (list_meta->num > 0) {
        last = list_meta->segments + (list_meta->num-1);
        if (check) {
            /* meaningless length */
            if (len <= 0) return -1;
            /* overlaps with last segment */
            if (last->index + last->len > index) return -1;
        }

        /* merge if continuous. */ 
        if (last->index + last->len == index &&
                last->type == type) {
            last->len += len;
            list_meta->len += len;
            return 0;
        }
    }

    listMetaMakeRoomFor(list_meta,list_meta->num+1);
   
    cur = list_meta->segments + list_meta->num++;
    cur->index = index;
    cur->len = len;
    cur->type = type;
    cur->reserved = 0;
    list_meta->len += len;

    return 0;
}

int listMetaAppendSegment(listMeta *list_meta, int type, long index,
        long len) {
    return listMetaAppendSegment_(list_meta,type,index,len,1);
}

int listMetaAppendSegmentWithoutCheck(listMeta *list_meta, int type,
        long index, long len) {
    return listMetaAppendSegment_(list_meta,type,index,len,0);
}

void listMetaDefrag(listMeta *list_meta) {
    int idx = 0, merged_idx = 0;
    segment *seg, *merged_seg;

    if (list_meta->num < 2) return;

    for (idx = 1; idx < list_meta->num; idx++) {
        seg = list_meta->segments+idx;
        merged_seg = list_meta->segments + merged_idx;

        if (seg->len == 0)  {
            /* directly skip empty segment */
        } else if (merged_seg->index + merged_seg->len == seg->index &&
            merged_seg->type == seg->type) {
            merged_seg->len += seg->len;
        } else {
            merged_idx++;
            merged_seg = list_meta->segments + merged_idx;
            memcpy(merged_seg,seg,sizeof(segment));
        }
    }
    list_meta->num = merged_idx+1;
}

/* [left right) */
void listMetaSearchOverlaps(listMeta *list_meta, segment *seg,
        int *left, int *right) {
    segment *cur;
    int l, r, m;

    l = 0, r = list_meta->num;
    while (l < r) {
        m = l + (r - l)/2;
        cur = list_meta->segments + m;
        if (cur->index + cur->len > seg->index) {
            r = m;
        } else {
            l = m+1;
        }
    }
    *left = l;

    l = 0, r = list_meta->num;
    while (l < r) {
        m = l + (r - l)/2;
        cur = list_meta->segments + m;
        if (cur->index < seg->index + seg->len) {
            l = m+1;
        } else {
            r = m;
        }
    }
    *right = l;
}

static int insegment(segment *seg, long index) {
    return seg->index <= index && index < seg->index + seg->len;
}

// #define SEGMENT_MAX_PADDING 32
#define SEGMENT_MAX_PADDING 0
listMeta *listMetaCalculateSwapInMeta(listMeta *list_meta, listMeta *req_meta) {
    listMeta *swap_meta = listMetaCreate();

    //TODO remove when production ready
    serverAssert(listMetaIsValid(list_meta,1));
    serverAssert(listMetaIsValid(req_meta,0));

    for (int i = 0; i < req_meta->num; i++) {
        int left, right;
        segment *req_seg = req_meta->segments + i;

        listMetaSearchOverlaps(list_meta, req_seg, &left, &right);

        for (int j = left; j < right; j++) {
            segment *list_seg = list_meta->segments + j;
            long left_index, right_index;

            if (list_seg->type == SEGMENT_TYPE_HOT)
                continue;

            /* if list cold segment is not much bigger than request segment,
             * then swapin the whole segment to reduce list segments */ 
            if (req_seg->index - list_seg->index <= SEGMENT_MAX_PADDING) {
                left_index = list_seg->index;
            } else {
                left_index = req_seg->index;
            }

            if (list_seg->index+list_seg->len-req_seg->index-req_seg->len
                    <= SEGMENT_MAX_PADDING) {
                right_index = list_seg->index + list_seg->len;
            } else {
                right_index = req_seg->index + req_seg->len;
            }

            listMetaAppendSegment(swap_meta,SEGMENT_TYPE_COLD,
                    left_index,right_index-left_index);
        }
    }

    listMetaDefrag(swap_meta);

    return swap_meta;
}

/* Swap out from middle to boundry (boundray are accessed more frequent) */
listMeta *listMetaCalculateSwapOutMeta(listMeta *list_meta) {
    listMeta *swap_meta = listMetaCreate();
    long max_eles;
    int l = (list_meta->num-1)/2, r = l+1, x, select_right;

    max_eles = server.swap_evict_step_max_memory/DEFAULT_LIST_FIELD_SIZE;
    if (max_eles > server.swap_evict_step_max_subkeys)
        max_eles = server.swap_evict_step_max_subkeys;

    while (max_eles > 0 && (l >= 0 || r < list_meta->num)) {
        long index, len;
        segment *seg;

        if (r >= list_meta->num) {
            x = l--;
            select_right = 0;
        } else if (l < 0) {
            x = r++;
            select_right = 1;
        } else if (l+1 > list_meta->num - r) {
            x = l--;
            select_right = 0;
        } else {
            x = r++;
            select_right = 1;
        }

        seg = list_meta->segments + x;
        if (seg->type == SEGMENT_TYPE_COLD)
            continue;

        len = seg->len <= max_eles ? seg->len : max_eles;
        max_eles -= len;

        if (select_right) {
            index = seg->index;
        } else {
            index = seg->index + seg->len - len;
        }

        listMetaAppendSegmentWithoutCheck(swap_meta,SEGMENT_TYPE_HOT,index,len);
    }

    /* By now, segments are ordered in zig-zag style, sort to normalize it. */
    qsort(swap_meta->segments,swap_meta->num,sizeof(segment),
            sortSegmentByIndex);

    listMetaDefrag(swap_meta);

    return swap_meta;
}

/* Return corresponding index in memlist to logical index, called when swap
 * finished, need to gurantee index is inside memlist when swap finish.
 * although we can binary search segment containing index, but still needs
 * to traverse to get corresponding midx. */
long listMetaGetMidx(listMeta *list_meta, long index) {
    segment *seg;
    long midx = 0, i, traversed = 0;

    for (i = 0; i < list_meta->num; i++) {
        seg = list_meta->segments+i;

        if (traversed + seg->len <= index) {
            traversed += seg->len;
            if (seg->type == SEGMENT_TYPE_HOT)
                midx += seg->len;
        } else {
            if (seg->type == SEGMENT_TYPE_HOT)
                midx += index-traversed;
            break;
        }
    }

    return midx;
}

segment *listMetaFirstSegment(listMeta *list_meta) {
    return list_meta->num <= 0 ? NULL : list_meta->segments;
}

segment *listMetaLastSegment(listMeta *list_meta) {
    return list_meta->num <= 0 ? NULL : list_meta->segments+list_meta->num-1;
}

/* Align delta with main meta */
listMeta *listMetaAlign(listMeta *main, listMeta *delta) {
    segment *cur;
    long prev_index, prev_len, next_index;
    listMeta *result = listMetaCreate();
    segment *first = listMetaFirstSegment(main),
            *delta_first = listMetaFirstSegment(delta),
            *last = listMetaLastSegment(main),
            *delta_last = listMetaLastSegment(delta);

    serverAssert(first->index <= delta_first->index);
    serverAssert(last->index+last->len >= delta_last->index+delta_last->len);

    prev_index = first->index;
    prev_len = 0;

    for (int i = 0; i < delta->num; i++) {
        cur = delta->segments+i;
        serverAssert(cur->type == SEGMENT_TYPE_HOT);
        next_index = prev_index + prev_len;
        if (next_index < cur->index) {
            listMetaAppendSegment(result,SEGMENT_TYPE_COLD,next_index,
                    cur->index-next_index);
        } else {
            serverAssert(next_index == cur->index);
        }
        listMetaAppendSegment(result,cur->type,cur->index,cur->len);
        prev_index = cur->index;
        prev_len = cur->len;
    }

    next_index = prev_index + prev_len;
    if (next_index < last->index+last->len) {
        listMetaAppendSegment(result,SEGMENT_TYPE_COLD,next_index,
                last->index+last->len-next_index);
    }

    return result;
}

/* update one of continous list meta at index to target type, may merge
 * with left segment.
 * return 1 if updated, 0 if nop, -1 if index out of range. */
static int listMetaUpdate(listMeta *list_meta, long index, int type) {
    segment *cur, *prev, *next;
    int l, r, m;

    serverAssert(listMetaIsValid(list_meta,1));

    l = 0, r = list_meta->num;
    while (l < r) {
        m = l + (r - l)/2;
        cur = list_meta->segments + m;
        if (cur->index + cur->len > index) {
            r = m;
        } else {
            l = m+1;
        }
    }

    if (l == list_meta->num) return -1;

    cur = list_meta->segments+l;
    if (!insegment(cur,index)) return -1;

    if (cur->type == type) return 0;

    listMetaMakeRoomFor(list_meta,list_meta->num+2);
    cur = list_meta->segments+l;

    if (cur->index == index) {
        prev = l > 0 ? list_meta->segments+l-1 : NULL;
        if (prev && prev->type == type) {
            /* merge with prev segment */
            prev->len++;
            cur->len--;
            cur->index++;
            /* remove current segment if it became empty */
            if (cur->len == 0) {
                memmove(cur,cur+1,(list_meta->num-l-1)*sizeof(segment));
                list_meta->num--;
            }
        } else {
            /* cant merge with prev segment */
            if (cur->len == 1) {
                cur->type = type;
            } else {
                /* split cur segment to 2 */
                memmove(cur+1,cur,(list_meta->num-l)*sizeof(segment));
                list_meta->num++;
                cur->type = type;
                cur->len = 1;
                next = cur+1;
                next->index = index+1;
                next->len--;
            }
        }
    } else {
        if (cur->len == 2) {
            /* split to 2 segment */
            memmove(cur+1,cur,(list_meta->num-l)*sizeof(segment));
            list_meta->num++;
            cur->len--;
            next = cur+1;
            next->index = index;
            next->len = 1;
            next->type = type;
        } else {
            /* split to 3 segment */
            memmove(cur+2,cur,(list_meta->num-l)*sizeof(segment));
            list_meta->num += 2;
            cur->len = index-cur->index;
            next = cur+1;
            next->index = index;
            next->len = 1;
            next->type = type;
            next = cur+2;
            next->len = next->index+next->len-index-1;
            next->index = index+1;
        }
    }

    return 1;
}

sds listMetaDump(sds result, listMeta *lm) {
    result = sdscatprintf(result,"(len=%ld,num=%ld,cap=%ld,segments=[",
            lm->len,lm->num,lm->capacity);
    for (int i = 0; i < lm->num; i++) {
        segment *seg = lm->segments+i;
        result = sdscatprintf(result,"%s:%ld:%ld,",
                seg->type == SEGMENT_TYPE_HOT ? "hot":"cold",
                (long)seg->index,seg->len);
    }
    sdscatfmt(result,"])");
    return result;
}

/* Meta list */
typedef struct metaList {
    listMeta *meta;
    robj *list;
} metaList;

typedef struct metaListIterator {
    listMetaIterator meta_iter[1];
    listTypeIterator *list_iter; /* own */
    listTypeEntry list_entry[1];
} metaListIterator;

void metaListIterInit(metaListIterator *iter, metaList *ml) {
    listMetaIteratorInitWithType(iter->meta_iter,ml->meta,SEGMENT_TYPE_HOT);
    iter->list_iter = listTypeInitIterator(ml->list,0,LIST_TAIL);
    listTypeNext(iter->list_iter,iter->list_entry);
}

void metaListIterDeinit(metaListIterator *iter) {
    if (iter->list_iter) {
        listTypeReleaseIterator(iter->list_iter);
        iter->list_iter = NULL;
    }
    listMetaIteratorDeinit(iter->meta_iter);
}

void metaListIterNext(metaListIterator *iter) {
    listTypeNext(iter->list_iter,iter->list_entry);
    listMetaIterNext(iter->meta_iter);
}

long metaListIterCur(metaListIterator *iter, int *segtype, robj **value) {
    if (value) *value = listTypeGet(iter->list_entry);
    return listMetaIterCur(iter->meta_iter,segtype);
}

int metaListIterFinished(metaListIterator *iter) {
    return listMetaIterFinished(iter->meta_iter);
}

int metaListIsValid(metaList *ml, int continuous) {
    if (!listMetaIsValid(ml->meta,continuous)) return 0;
    return (long)listTypeLength(ml->list) <= ml->meta->len;
}

metaList *metaListCreate() {
    metaList *ml = zmalloc(sizeof(metaList));
    ml->list = createQuicklistObject();
    ml->meta = listMetaCreate();
    return ml;
}

void metaListFree(metaList *ml) {
    if (ml->meta) {
        listMetaFree(ml->meta);
        ml->meta = NULL;
    }
    if (ml->list) {
        decrRefCount(ml->list);
        ml->list = NULL;
    }
}

metaList *metaListBuild(MOVE listMeta *meta, MOVE robj *list) {
    metaList *ml = zmalloc(sizeof(metaList));
    ml->list = list;
    ml->meta = meta;
    return ml;
}

void metaListDestroy(metaList *ml) {
    if (ml == NULL) return;
    if (ml->list) {
        decrRefCount(ml->list);
        ml->list = NULL;
    }
    if (ml->meta) {
        listMetaFree(ml->meta);
        ml->meta = NULL;
    }
    zfree(ml);
}

sds metaListDump(sds result, metaList *ml) {
    metaListIterator iter;
    metaListIterInit(&iter,ml);
    result = sdscatprintf(result,"(len=%ld,list=[",listTypeLength(ml->list));
    while (!metaListIterFinished(&iter)) {
        int segtype;
        robj *val;
        long ridx = metaListIterCur(&iter,&segtype,&val);
        if (val->encoding == OBJ_ENCODING_INT) {
            result = sdscatprintf(result,"ridx=%ld/val:%ld,",ridx,(long)val->ptr);
        } else {
            result = sdscatprintf(result,"ridx=%ld/val:%s,",ridx,(char*)val->ptr);
        }
        metaListIterNext(&iter);
    }
    result = sdscatprintf(result,"])");
    metaListIterDeinit(&iter);
    return result;
}

void metaListFeedStart(metaList *main, segment *start) {
    serverAssert(listMetaEmpty(main->meta) && listTypeLength(main->list));
    listMetaAppendSegmentWithoutCheck(main->meta,start->type,start->index,0);
}

/* Feed metaList with hot element. */
void metaListFeedElement(metaList *main, int ridx, robj *ele) {
    listMeta *meta = main->meta;
    robj *list = main->list;
    segment *last;
    long next_ridx;

    serverAssert(ele);
    serverAssert(meta->num > 0);
    
    last = listMetaLastSegment(meta);
    next_ridx = last->index+last->len;

    serverAssert(ridx >= next_ridx);

    if (ridx > next_ridx) {
        listMetaAppendSegment(meta,SEGMENT_TYPE_COLD,next_ridx,
                ridx-next_ridx);
        listMetaAppendSegmentWithoutCheck(meta,SEGMENT_TYPE_HOT,ridx,0);
        last = listMetaLastSegment(meta);
    }

    last->len++;
    listTypePush(list,ele,LIST_TAIL);
}

void metaListFeedEnd(metaList *main, segment *end) {
    listMeta *meta = main->meta;
    segment *last = listMetaLastSegment(meta);

    if (end->len <= 0) return;

    if (meta->num > 0) {
        last = meta->segments+meta->num-1;
        serverAssert(last->index+last->len <= end->index+end->len);
    }

    if (end->type == SEGMENT_TYPE_HOT) {
        /* All elements must have already feeded to main if ending with
         * hot segment. */
        serverAssert(last->index+last->len == end->index+end->len);
    } else {
        /* Append or expend if ending segment is cold */
        if (last == NULL || last->type == SEGMENT_TYPE_HOT) {
            if (last->index+last->len < end->index+end->len) {
                long index = last->index+last->len;
                long len = end->index+end->len - index;
                listMetaAppendSegment(meta,SEGMENT_TYPE_COLD,index,len);
            }
        } else {
            last->len = end->index+end->len-last->index;
        }
    }
}

static void objectSwap(robj *lhs, robj *rhs) {
    void *tmp;
    serverAssert(lhs->type == rhs->type);
    serverAssert(lhs->encoding == rhs->encoding);
    tmp = lhs->ptr;
    lhs->ptr = rhs->ptr;
    rhs->ptr = tmp;
}

static void listMetaSwap(listMeta *lhs, listMeta *rhs) {
    listMeta tmp;
    tmp = *lhs;
    *lhs = *rhs;
    *rhs = tmp;
}

void metaListSwap(metaList *lhs, metaList *rhs) {
    objectSwap(lhs->list,rhs->list);
    listMetaSwap(lhs->meta,rhs->meta);
}

long metaListLen(metaList *ml, int type) {
    switch (type) {
    case SEGMENT_TYPE_HOT:
        return listTypeLength(ml->list);
    case SEGMENT_TYPE_BOTH:
        return ml->meta->len;
    case SEGMENT_TYPE_COLD:
        return ml->meta->len - listTypeLength(ml->list);
    default:
        return -1;
    }
}

int metaListInsert(metaList *main, long ridx, robj *value) {
    int insert = 0, segtype;
    metaListIterator iter;

    metaListIterInit(&iter,main);
    while (!metaListIterFinished(&iter)) {
        long curidx = metaListIterCur(&iter,&segtype,NULL);
        /* ridx is hot, not inserted */
        if (curidx == ridx) break;
        if (curidx > ridx) {
            insert = 1;
            break;
        }
        metaListIterNext(&iter);
    }

    if (insert || metaListIterFinished(&iter)) {
        listMetaUpdate(main->meta,ridx,SEGMENT_TYPE_HOT);
        listTypeInsert(iter.list_entry,value,LIST_HEAD);
    }
    metaListIterDeinit(&iter);

    return insert;
}

int metaListDelete(metaList *main, long ridx) {
    int delete = 0, segtype;
    metaListIterator iter;

    metaListIterInit(&iter,main);
    while (!metaListIterFinished(&iter)) {
        long curidx = metaListIterCur(&iter,&segtype,NULL);
        if (curidx == ridx) {
            delete = 1;
            break;
        }
        if (curidx > ridx) break;
        metaListIterNext(&iter);
    }

    if (delete) {
        listMetaUpdate(main->meta,ridx,SEGMENT_TYPE_COLD);
        listTypeDelete(iter.list_iter,iter.list_entry);
    }
    metaListIterDeinit(&iter);

    return delete;
}

long metaListMerge(metaList *main, metaList *delta) {
    long merged = 0, ridx;
    int segtype;
    metaListIterator delta_iter;
    robj *ele;

    serverAssert(metaListIsValid(main,1));
    serverAssert(metaListIsValid(delta,0));

    /* always merge small inst into big one */
    if (metaListLen(main,SEGMENT_TYPE_HOT) <
            metaListLen(delta,SEGMENT_TYPE_HOT)) {
        listMeta *orig_delta_meta = delta->meta;
        delta->meta = listMetaAlign(main->meta, orig_delta_meta);
        listMetaFree(orig_delta_meta);
        metaListSwap(main,delta);
        
        /* //TODO remove */
        /* sds main_dump = listMetaDump(sdsempty(),main->meta); */
        /* sds delta_dump = listMetaDump(sdsempty(),delta->meta); */
        /* serverLog(LL_WARNING,"[list] align: \n  main:%s\n  delta:%s\n", */
                /* main_dump,delta_dump); */
        /* sdsfree(main_dump); */
        /* sdsfree(delta_dump); */
    }

    metaListIterInit(&delta_iter,delta);
    while (!metaListIterFinished(&delta_iter)) {
        ridx = metaListIterCur(&delta_iter,&segtype,&ele);
        serverAssert(segtype == SEGMENT_TYPE_HOT);
        merged += metaListInsert(main,ridx,ele);
        metaListIterNext(&delta_iter);

        /* //TODO remove */
        /* sds main_dump = listMetaDump(sdsempty(),main->meta); */
        /* sds delta_dump = listMetaDump(sdsempty(),delta->meta); */
        /* serverLog(LL_WARNING,"[list] insert-%ld: \n  main:%s\n  delta:%s\n", */
                /* ridx,main_dump,delta_dump); */
        /* sdsfree(main_dump); */
        /* sdsfree(delta_dump); */
    }

    listMetaDefrag(main->meta);
    metaListIterDeinit(&delta_iter);

    return merged;
}

int metaListExclude(metaList *main, listMeta *delta) {
    long excluded = 0;
    listMetaIterator delta_iter;

    listMetaIteratorInit(&delta_iter, delta);
    while (!listMetaIterFinished(&delta_iter)) {
        int segtype;
        long ridx = listMetaIterCur(&delta_iter,&segtype);
        serverAssert(segtype == SEGMENT_TYPE_COLD);
        excluded += metaListDelete(main,ridx);
        listMetaIterNext(&delta_iter);

        /* //TODO remove */
        /* sds main_dump = listMetaDump(sdsempty(),main->meta); */
        /* sds delta_dump = listMetaDump(sdsempty(),delta); */
        /* serverLog(LL_WARNING,"[list] exclude-%ld: \n  main:%s\n  delta:%s\n", */
                /* ridx,main_dump,delta_dump); */
        /* sdsfree(main_dump); */
        /* sdsfree(delta_dump); */
    }
    listMetaDefrag(main->meta);
    listMetaIteratorDeinit(&delta_iter);

    return excluded;
}

/* List swap data */

/* List rdb save */

/* List rdb load */


#ifdef REDIS_TEST

void listMetaReset(listMeta *lm) {
    lm->len = 0;
    lm->num = 0;
}

void listMetaPush6Seg(listMeta *lm) {
    /* 0~9(HOT) | 10~19(COLD) | 20~29(HOT) | 30~39(COLD) | 40~49(HOT) | 50~59(COLD) */
    listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,0,10);
    listMetaAppendSegment(lm,SEGMENT_TYPE_COLD,10,10);
    listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,20,10);
    listMetaAppendSegment(lm,SEGMENT_TYPE_COLD,30,10);
    listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,40,10);
    listMetaAppendSegment(lm,SEGMENT_TYPE_COLD,50,10);
}

void metaListPopulateList(metaList *ml) {
    int segtype;
    long ridx;
    listMetaIterator iter;

    listMetaIteratorInit(&iter,ml->meta);
    while (!listMetaIterFinished(&iter)) {
        ridx = listMetaIterCur(&iter,&segtype);
        if (segtype == SEGMENT_TYPE_HOT) {
            listTypePush(ml->list,createStringObjectFromLongLong(ridx),
                    LIST_TAIL);
        }
        listMetaIterNext(&iter);
    }
}

void metaListPush6Seg(metaList *ml) {
    listMetaPush6Seg(ml->meta);
    metaListPopulateList(ml);
}

void turnListMeta2Type(listMeta *lm, int type) {
    for (int i = 0; i < lm->num; i++) {
        lm->segments[i].type = type;
    }
}

void initServerConfig(void);
int swapListTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;

    TEST("list: init") {
        initTestRedisServer();
    }

    TEST("list-meta: basics & iterator") {
        listMeta *lm = listMetaCreate();
        test_assert(listMetaIsValid(lm,1));
        test_assert(listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,0,2) == 0);
        test_assert(listMetaIsValid(lm,1));
        test_assert(lm->num == 1 && lm->len == 2);
        test_assert(listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,2,2) == 0);
        test_assert(lm->num == 1 && lm->len == 4);
        test_assert(listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,2,2) == -1);
        test_assert(listMetaAppendSegment(lm,SEGMENT_TYPE_COLD,2,2) == -1);
        test_assert(listMetaAppendSegment(lm,SEGMENT_TYPE_COLD,4,2) == 0);
        test_assert(lm->num == 2 && lm->len == 6);
        test_assert(listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,10,2) == 0);
        test_assert(listMetaAppendSegment(lm,SEGMENT_TYPE_COLD,20,2) == 0);
        test_assert(listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,30,2) == 0);
        test_assert(listMetaAppendSegment(lm,SEGMENT_TYPE_COLD,40,2) == 0);
        test_assert(lm->capacity == 8);
        test_assert(listMetaIsValid(lm,1));
        listMetaFree(lm);
    }

    TEST("list-meta: iterator") {
        listMetaIterator iter;
        listMeta *lm = listMetaCreate();
        listMetaIteratorInit(&iter,lm);
        test_assert(listMetaIterFinished(&iter));
        listMetaIteratorDeinit(&iter);
        listMetaAppendSegmentWithoutCheck(lm,SEGMENT_TYPE_HOT,0,0);
        listMetaAppendSegmentWithoutCheck(lm,SEGMENT_TYPE_HOT,0,0);
        listMetaIteratorInit(&iter,lm);
        test_assert(listMetaIterFinished(&iter));
        listMetaIteratorDeinit(&iter);
        listMetaAppendSegmentWithoutCheck(lm,SEGMENT_TYPE_HOT,0,2);
        listMetaAppendSegmentWithoutCheck(lm,SEGMENT_TYPE_COLD,2,2);
        listMetaAppendSegmentWithoutCheck(lm,SEGMENT_TYPE_HOT,4,0);
        listMetaAppendSegmentWithoutCheck(lm,SEGMENT_TYPE_COLD,4,1);
        listMetaIteratorInit(&iter,lm);
        test_assert(!listMetaIterFinished(&iter) && listMetaIterCur(&iter,NULL) == 0);
        listMetaIterNext(&iter);
        test_assert(!listMetaIterFinished(&iter) && listMetaIterCur(&iter,NULL) == 1);
        listMetaIterNext(&iter);
        test_assert(!listMetaIterFinished(&iter) && listMetaIterCur(&iter,NULL) == 2);
        listMetaIterNext(&iter);
        test_assert(!listMetaIterFinished(&iter) && listMetaIterCur(&iter,NULL) == 3);
        listMetaIterNext(&iter);
        test_assert(!listMetaIterFinished(&iter) && listMetaIterCur(&iter,NULL) == 4);
        listMetaIterNext(&iter);
        test_assert(listMetaIterFinished(&iter));
        listMetaIteratorDeinit(&iter);
        listMetaFree(lm);
    }

    TEST("list-meta: defrag") {
        segment *seg;
        listMeta *lm = listMetaCreate();
        lm->segments = zrealloc(lm->segments,8*sizeof(segment));
        lm->capacity = 8;

        seg = lm->segments + lm->num++;
        seg->type = SEGMENT_TYPE_HOT;
        seg->index = 10;
        seg->len = 2;
        seg = lm->segments + lm->num++;
        seg->type = SEGMENT_TYPE_HOT;
        seg->index = 12;
        seg->len = 0;
        seg = lm->segments + lm->num++;
        seg->type = SEGMENT_TYPE_HOT;
        seg->index = 12;
        seg->len = 2;

        seg = lm->segments + lm->num++;
        seg->type = SEGMENT_TYPE_COLD;
        seg->index = 14;
        seg->len = 2;

        seg = lm->segments + lm->num++;
        seg->type = SEGMENT_TYPE_COLD;
        seg->index = 20;
        seg->len = 2;
        seg = lm->segments + lm->num++;
        seg->type = SEGMENT_TYPE_COLD;
        seg->index = 22;
        seg->len = 2;

        lm->len = 10;

        listMetaDefrag(lm);

        test_assert(lm->len == 10);
        test_assert(lm->num == 3);
        listMetaFree(lm);
    }

    TEST("list-meta: normalize from request") {
        listMeta *qm;

        range ltrim1[2] = {{0,1},{-1,0}};
        qm = listMetaNormalizeFromRequest(2,ltrim1,4);
        test_assert(qm == NULL);


        range ltrim2[2] = {{0,1},{-2,-1}};
        qm = listMetaNormalizeFromRequest(2,ltrim2,4);
        test_assert(listMetaIsValid(qm,0));
        test_assert(qm->num == 2 && qm->len == 4);
        test_assert(qm->segments[0].index == 0 && qm->segments[0].len == 2); 
        test_assert(qm->segments[1].index == 2 && qm->segments[0].len == 2); 
        listMetaFree(qm);

        range within_range[2] = { {0,1},{-5,-4}};
        qm = listMetaNormalizeFromRequest(2,within_range,4);
        test_assert(qm->num == 2 && qm->len == 3);
        test_assert(qm->segments[0].index == 0 && qm->segments[0].len == 2); 
        test_assert(qm->segments[1].index == 0 && qm->segments[1].len == 1); 
        listMetaFree(qm);

        range exceed_range[2] = { {0,1},{-5,-5}};
        qm = listMetaNormalizeFromRequest(2,exceed_range,4);
        test_assert(qm == NULL);
    }

    TEST("list-meta: search overlaps") {
        segment seg = {SEGMENT_TYPE_HOT,0,0,0};
        int left, right;
        listMeta *lm = listMetaCreate();

        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 0 && right == 0);

        seg.len = 1;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 0 && right == 0);

        listMetaAppendSegment(lm,SEGMENT_TYPE_COLD,0,1);
        seg.len = 1;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 0 && right == 1);

        seg.len = 0;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 0 && right == 0);

        seg.len = 3;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 0 && right == 1);

        listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,1,3);
        seg.len = 1;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 0 && right == 1);

        seg.len = 2;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 0 && right == 2);

        seg.len = 4;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 0 && right == 2);

        seg.index = 2, seg.len = 1;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 1 && right == 2);

        seg.index = 2, seg.len = 2;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 1 && right == 2);

        seg.index = 2, seg.len = 3;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 1 && right == 2);

        listMetaAppendSegment(lm,SEGMENT_TYPE_COLD,4,4);
        seg.index = 0, seg.len = 3;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 0 && right == 2);

        seg.index = 1, seg.len = 3;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 1 && right == 2);

        seg.index = 1, seg.len = 4;
        listMetaSearchOverlaps(lm,&seg,&left,&right);
        test_assert(left == 1 && right == 3);

        listMetaFree(lm);
    }

#undef SEGMENT_MAX_PADDING
#define SEGMENT_MAX_PADDING 0

    TEST("list-meta: calculate swap in meta") {
        listMeta *lm = listMetaCreate(), *qm, *sm;
        /* hot */
        listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,0,4);
        range ltrim[2] = { {0,0}, {-1,-1} };
        qm = listMetaNormalizeFromRequest(2,ltrim,4);
        sm = listMetaCalculateSwapInMeta(lm,qm);
        test_assert(listMetaEmpty(sm));
        listMetaFree(sm);
        /* cold */
        listMetaReset(lm);
        listMetaAppendSegment(lm,SEGMENT_TYPE_COLD,0,4);
        sm = listMetaCalculateSwapInMeta(lm,qm);
        test_assert(sm->len == 2 && sm->num == 2);
        listMetaFree(sm);
        /* warm */
        listMetaReset(lm);
        listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,0,2);
        listMetaAppendSegment(lm,SEGMENT_TYPE_COLD,2,2);
        sm = listMetaCalculateSwapInMeta(lm,qm);
        test_assert(sm->len == 1 && sm->num == 1);
        listMetaFree(sm);
        listMetaFree(qm);
        /* complex */
        listMetaReset(lm);
        listMetaPush6Seg(lm);
        
        range req1[1] = {{15,44}};
        qm = listMetaNormalizeFromRequest(1,req1,60);
        sm = listMetaCalculateSwapInMeta(lm,qm);
        test_assert(sm->num == 2 && sm->len == 15);
        test_assert(sm->segments[0].index == 15 && sm->segments[0].len == 5);
        test_assert(sm->segments[1].index == 30 && sm->segments[1].len == 10);
        listMetaFree(qm), listMetaFree(sm);

        range req2[5] = { {5,14}, {15,24}, {25,29}, {30,54}, {55,59}};
        qm = listMetaNormalizeFromRequest(5,req2,60);
        sm = listMetaCalculateSwapInMeta(lm,qm);
        test_assert(sm->num == 3 && sm->len == 30);
        test_assert(sm->segments[0].index == 10 && sm->segments[0].len == 10);
        test_assert(sm->segments[1].index == 30 && sm->segments[1].len == 10);
        test_assert(sm->segments[2].index == 50 && sm->segments[2].len == 10);
        listMetaFree(qm), listMetaFree(sm);

        listMetaFree(lm);
    }

    TEST("list-meta: calculate swap out meta") {
        listMeta *lm = listMetaCreate(), *sm;
        server.swap_evict_step_max_memory = 1000000; /* not limit */
        listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,0,10);

        server.swap_evict_step_max_subkeys = 5;
        sm = listMetaCalculateSwapOutMeta(lm);
        test_assert(sm->num == 1 && sm->len == 5);
        test_assert(sm->segments[0].index == 5 && sm->segments[0].len == 5);
        listMetaFree(sm);

        server.swap_evict_step_max_subkeys = 20;
        sm = listMetaCalculateSwapOutMeta(lm);
        test_assert(sm->num == 1 && sm->len == 10);
        test_assert(sm->segments[0].index == 0 && sm->segments[0].len == 10);
        listMetaFree(sm);

        listMetaReset(lm);
        listMetaPush6Seg(lm);

        server.swap_evict_step_max_subkeys = 10;
        sm = listMetaCalculateSwapOutMeta(lm);
        test_assert(sm->num == 1 && sm->len == 10);
        test_assert(sm->segments[0].index == 20 && sm->segments[0].len == 10);
        listMetaFree(sm);

        server.swap_evict_step_max_subkeys = 15;
        sm = listMetaCalculateSwapOutMeta(lm);
        test_assert(sm->num == 2 && sm->len == 15);
        test_assert(sm->segments[0].index == 20 && sm->segments[0].len == 10);
        test_assert(sm->segments[1].index == 40 && sm->segments[1].len == 5);
        listMetaFree(sm);

        server.swap_evict_step_max_subkeys = 40;
        sm = listMetaCalculateSwapOutMeta(lm);
        test_assert(sm->num == 3 && sm->len == 30);
        listMetaFree(sm);
        listMetaFree(lm);
    }

    TEST("list-meta: get midx(index in memlist)") {
        listMeta *lm = listMetaCreate();
        listMetaPush6Seg(lm);
        test_assert(listMetaGetMidx(lm,5) == 5);
        test_assert(listMetaGetMidx(lm,15) == 10);
        test_assert(listMetaGetMidx(lm,20) == 10);
        test_assert(listMetaGetMidx(lm,45) == 25);
        test_assert(listMetaGetMidx(lm,55) == 30);
        listMetaFree(lm);
    }

    TEST("meta-list: merge") {
        listMeta *meta = listMetaCreate();
        robj *list = createQuicklistObject();
        metaList *main = metaListBuild(meta,list);

        metaListPush6Seg(main);
        
        /* skip if overlaps with main hot */
        range req1[1] = { {5,5} };
        listMeta *meta1 = listMetaNormalizeFromRequest(1,req1,60);
        robj *list1 = createQuicklistObject();
        metaList *delta1 = metaListBuild(meta1,list1);
        metaListPopulateList(delta1);
        test_assert(metaListMerge(main,delta1) == 0);
        test_assert(meta->len == 60 && meta->num == 6 && listTypeLength(list) == 30);
        metaListDestroy(delta1);

        /* merge with hot */
        range req2[1] = { {10,11} };
        listMeta *meta2 = listMetaNormalizeFromRequest(1,req2,60);
        robj *list2 = createQuicklistObject();
        metaList *delta2 = metaListBuild(meta2,list2);
        metaListPopulateList(delta2);
        test_assert(metaListMerge(main,delta2) == 2);
        test_assert(meta->len == 60 && meta->num == 6 && listTypeLength(list) == 32);
        metaListDestroy(delta2);

        /* merge and split */
        range req3[1] = { {14,15}, };
        listMeta *meta3 = listMetaNormalizeFromRequest(1,req3,60);
        robj *list3 = createQuicklistObject();
        metaList *delta3 = metaListBuild(meta3,list3);
        metaListPopulateList(delta3);
        test_assert(metaListMerge(main,delta3) == 2);
        test_assert(meta->len == 60 && meta->num == 8 && listTypeLength(list) == 34);
        metaListDestroy(delta3);

        /* complex overlap */
        range req4[3] = { {4,4}, {5,44}, {48,57} };
        listMeta *meta4 = listMetaNormalizeFromRequest(3,req4,60);
        robj *list4 = createQuicklistObject();
        metaList *delta4 = metaListBuild(meta4,list4);
        metaListPopulateList(delta4);
        test_assert(metaListMerge(main,delta4) == 7);
        test_assert(meta->len == 60 && meta->num == 2 && listTypeLength(list) == 58);
        metaListDestroy(delta4);

        metaListDestroy(main);
    }

    TEST("meta-list: exclude") {
        listMeta *meta = listMetaCreate();
        robj *list = createQuicklistObject();
        metaList *main = metaListBuild(meta,list);

        metaListPush6Seg(main);
        
        /* skip if overlaps with main cold */
        range req1[1] = { {10,11} };
        listMeta *meta1 = listMetaNormalizeFromRequest(1,req1,60);
        turnListMeta2Type(meta1,SEGMENT_TYPE_COLD);
        test_assert(metaListExclude(main,meta1) == 0);
        test_assert(meta->len == 60 && meta->num == 6 && listTypeLength(list) == 30);
        listMetaFree(meta1);

        /* exclude cold segment */
        range req2[1] = { {0,1} };
        listMeta *meta2 = listMetaNormalizeFromRequest(1,req2,60);
        turnListMeta2Type(meta2,SEGMENT_TYPE_COLD);
        test_assert(metaListExclude(main,meta2) == 2);
        test_assert(meta->len == 60 && meta->num == 7 && listTypeLength(list) == 28);
        listMetaFree(meta2);

        /* exclude and split */
        range req3[1] = { {25,26} };
        listMeta *meta3 = listMetaNormalizeFromRequest(1,req3,60);
        turnListMeta2Type(meta3,SEGMENT_TYPE_COLD);
        test_assert(metaListExclude(main,meta3) == 2);
        test_assert(meta->len == 60 && meta->num == 9 && listTypeLength(list) == 26);
        listMetaFree(meta3);

        /* complex */
        range req4[3] = { {5,14}, {15,44}, {50,52} };
        listMeta *meta4 = listMetaNormalizeFromRequest(3,req4,60);
        turnListMeta2Type(meta4,SEGMENT_TYPE_COLD);
        test_assert(metaListExclude(main,meta4) == 18);
        test_assert(meta->len == 60 && meta->num == 5 && listTypeLength(list) == 8);
        listMetaFree(meta4);
        metaListDestroy(main);
    }

    return error;
}

#endif


