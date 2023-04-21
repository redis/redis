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

#define LIST_INITIAL_INDEX (LONG_MAX>>1)
#define LIST_MAX_INDEX LONG_MAX
#define LIST_MIN_INDEX 0

#define SEGMENT_TYPE_HOT 0
#define SEGMENT_TYPE_COLD 1
#define SEGMENT_TYPE_BOTH 2

typedef struct segment {
  int type;
  long index;
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

typedef void (*selectElementCallback)(long ridx, robj *ele, void *pd);

static inline long listGetInitialRidx(long index) {
    return index + LIST_INITIAL_INDEX;
}

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

    /* skip leading empty segments or unmatched */
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
    if (segidx == meta->num)
        iter->ridx = LIST_META_ITER_FINISHED;
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
    list_meta->len = 0;
    list_meta->num = 0;
    list_meta->capacity = LIST_META_CAPCITY_DEFAULT;
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

listMeta *listMetaNormalizeFromRequest(long ridx_shift, int num,
        range *ranges, long llen) {
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
        seg->index = r->start + ridx_shift;
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

#define LIST_META_STRICT_NOEMPTY (1U << 1)
#define LIST_META_STRICT_CONTINOUS (1U << 2)
#define LIST_META_STRICT_RIDX (1U << 3)

#define LITS_META_STRICT_SENSABLE_RIDX_MIN  (LONG_MAX>>2)
#define LITS_META_STRICT_SENSABLE_RIDX_MAX  ((LONG_MAX>>2) + (LONG_MAX>>1))

/* List meta segments are constinuous, req meta aren't . */ 
int listMetaIsValid(listMeta *list_meta, int strict) {
    segment *seg;
    long i, expected_len = 0, next_index = -1;
    int noempty = strict & LIST_META_STRICT_NOEMPTY,
        continuous = strict & LIST_META_STRICT_CONTINOUS,
        ridx = strict & LIST_META_STRICT_RIDX;

    for (i = 0; i < list_meta->num; i++) {
        seg = list_meta->segments + i;
        expected_len += seg->len;
        if (seg->len < 0) return 0;
        if (seg->len == 0 && noempty) return 0;
        if (ridx && (seg->index < LITS_META_STRICT_SENSABLE_RIDX_MIN || seg->index > LITS_META_STRICT_SENSABLE_RIDX_MAX)) return 0;
        if (next_index == -1 || 
                (continuous && next_index == seg->index) ||
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
            memmove(merged_seg,seg,sizeof(segment));
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

#ifdef REDIS_TEST
  #define SEGMENT_MAX_PADDING 0
#else
  #define SEGMENT_MAX_PADDING 32
#endif

listMeta *listMetaCalculateSwapInMeta(listMeta *list_meta, listMeta *req_meta) {
    listMeta *swap_meta = listMetaCreate();

    //TODO remove if prod ready
    serverAssert(listMetaIsValid(list_meta,
                LIST_META_STRICT_CONTINOUS|LIST_META_STRICT_NOEMPTY));
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

            listMetaAppendSegment(swap_meta,SEGMENT_TYPE_HOT,
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

    max_eles = server.swap_evict_step_max_memory/DEFAULT_LIST_ELE_SIZE;
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

        listMetaAppendSegmentWithoutCheck(swap_meta,SEGMENT_TYPE_COLD,index,len);
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

long listMetaGetRidxShift(listMeta *list_meta) {
    segment *first = listMetaFirstSegment(list_meta);
    if (first) {
        return first->index;
    } else {
        return listGetInitialRidx(0);
    }
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

    serverAssert(listMetaIsValid(list_meta,LIST_META_STRICT_CONTINOUS));

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

long listMetaLength(listMeta *list_meta, int type) {
    segment *seg;
    long len = 0;

    serverAssert(list_meta);

    switch (type) {
    case SEGMENT_TYPE_BOTH:
        return list_meta->len;
    case SEGMENT_TYPE_COLD:
    case SEGMENT_TYPE_HOT:
        for (int i = 0; i < list_meta->num; i++) {
            seg = list_meta->segments+i;
            if (seg->type == type) {
                len += seg->len;
            }
        }
        return len;
    default:
        serverPanic("unexpected list meta type");
        break;
    }
    return 0;
}

/* expand(if delta > 0) or shink(if delta < 0) hot segment */
void listMetaExtend(listMeta *list_meta, long head_delta, long tail_delta) {
    segment *first, *last;
    serverAssert(list_meta);

    serverAssert(list_meta->len+head_delta >= 0 && list_meta->len+tail_delta >= 0);

    list_meta->len += head_delta;
    list_meta->len += tail_delta;

    /* head */
    if (head_delta > 0) {
        first = listMetaFirstSegment(list_meta);
        if (first->type != SEGMENT_TYPE_HOT) {
            /* prepend a hot segment */
            listMetaMakeRoomFor(list_meta,list_meta->num+1);
            memmove(list_meta->segments+1,list_meta->segments,
                    list_meta->num*sizeof(segment));
            list_meta->num++;
            /* Note that first is invalid after makeroom */
            first = listMetaFirstSegment(list_meta);
            first->len = 0;
            first->type = SEGMENT_TYPE_HOT;
        }
        first->index -= head_delta;
        first->len += head_delta;
    } else if (head_delta == 0) {
        /* nop */
    } else {
        head_delta = -head_delta;
        while (head_delta) {
            first = listMetaFirstSegment(list_meta);
            serverAssert(first->type == SEGMENT_TYPE_HOT);
            if (head_delta < first->len) {
                first->index += head_delta;
                first->len -= head_delta;
                head_delta = 0;
            } else {
                head_delta -= first->len;
                memmove(list_meta->segments,list_meta->segments+1,
                        (list_meta->num-1)*sizeof(segment));
                list_meta->num--;
            }
        }
    }

    /* tail */
    if (tail_delta > 0) {
        last = listMetaLastSegment(list_meta);
        if (last->type != SEGMENT_TYPE_HOT) {
            /* append a hot segment */
            listMetaAppendSegmentWithoutCheck(list_meta,SEGMENT_TYPE_HOT,
                    last->index+last->len,0);
            last = listMetaLastSegment(list_meta);
        }
        last->len += tail_delta;
    } else if (tail_delta == 0) {
        /* nop */
    } else {
        tail_delta = -tail_delta;
        while (tail_delta) {
            last = listMetaLastSegment(list_meta);
            serverAssert(last->type == SEGMENT_TYPE_HOT);
            if (tail_delta < last->len) {
                last->len -= tail_delta;
                tail_delta = 0;
            } else {
                tail_delta -= last->len;
                list_meta->num--;
            }
        }
    }
}

listMeta *listMetaDup(listMeta *list_meta) {
    listMeta *lm = zmalloc(sizeof(listMeta));
    lm->len = list_meta->len;
    lm->num = list_meta->num;
    lm->capacity = list_meta->capacity;
    lm->segments = zmalloc(sizeof(segment)*list_meta->capacity);
    memcpy(lm->segments,list_meta->segments,sizeof(segment)*lm->num);
    return lm;
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
    result = sdscatfmt(result,"])");
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

int metaListIsValid(metaList *ml, int strict) {
    if (!listMetaIsValid(ml->meta,strict)) return 0;
    return (long)listTypeLength(ml->list) <= ml->meta->len;
}

metaList *metaListCreate() {
    metaList *ml = zmalloc(sizeof(metaList));
    ml->list = createQuicklistObject();
    ml->meta = listMetaCreate();
    return ml;
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
            result = sdscatprintf(result,"ridx:%ld|val:%ld,",ridx,(long)val->ptr);
        } else {
            result = sdscatprintf(result,"ridx:%ld|val:%s,",ridx,(char*)val->ptr);
        }
        metaListIterNext(&iter);
    }
    result = sdscatprintf(result,"])");
    metaListIterDeinit(&iter);
    return result;
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

    if (insert) {
        listMetaUpdate(main->meta,ridx,SEGMENT_TYPE_HOT);
        listTypeInsert(iter.list_entry,value,LIST_HEAD);
    } else if (metaListIterFinished(&iter)) {
        listMetaUpdate(main->meta,ridx,SEGMENT_TYPE_HOT);
        listTypePush(main->list,value,LIST_TAIL);
        insert = 1;
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

    serverAssert(metaListIsValid(main,LIST_META_STRICT_NOEMPTY|LIST_META_STRICT_CONTINOUS));
    serverAssert(metaListIsValid(delta,0));

    /* always merge small inst into big one */
    if (metaListLen(main,SEGMENT_TYPE_HOT) <
            metaListLen(delta,SEGMENT_TYPE_HOT)) {
        listMeta *orig_delta_meta = delta->meta;
        delta->meta = listMetaAlign(main->meta, orig_delta_meta);
        listMetaFree(orig_delta_meta);
        metaListSwap(main,delta);
        
#ifdef SWAP_LIST_DEBUG
        sds main_dump = listMetaDump(sdsempty(),main->meta);
        sds delta_dump = listMetaDump(sdsempty(),delta->meta);
        serverLog(LL_WARNING,"[list] align: \n  main:%s\n  delta:%s\n",
                main_dump,delta_dump);
        sdsfree(main_dump);
        sdsfree(delta_dump);
#endif
    }

    metaListIterInit(&delta_iter,delta);
    while (!metaListIterFinished(&delta_iter)) {
        robj *ele;

        ridx = metaListIterCur(&delta_iter,&segtype,&ele);
        serverAssert(segtype == SEGMENT_TYPE_HOT);
        merged += metaListInsert(main,ridx,ele);
        metaListIterNext(&delta_iter);

#ifdef SWAP_LIST_DEBUG
        sds ele_dump = ele->encoding == OBJ_ENCODING_INT ? 
            sdsfromlonglong((long)ele->ptr) : sdsdup(ele->ptr);
        sds main_dump = listMetaDump(sdsempty(),main->meta);
        sds delta_dump = listMetaDump(sdsempty(),delta->meta);
        serverLog(LL_WARNING,"[list] insert-%ld:%s\n  main:%s\n  delta:%s\n",
                ridx,ele_dump,main_dump,delta_dump);
        sdsfree(ele_dump);
        sdsfree(main_dump);
        sdsfree(delta_dump);
#endif

        decrRefCount(ele);
    }

    listMetaDefrag(main->meta);
    metaListIterDeinit(&delta_iter);

    return merged;
}

int metaListSelect(metaList *main, listMeta *delta, selectElementCallback cb, void *pd) {
    long selected = 0;
    metaListIterator main_iter;
    listMetaIterator delta_iter;

    metaListIterInit(&main_iter,main);
    listMetaIteratorInit(&delta_iter,delta);

    while (!listMetaIterFinished(&delta_iter) && !metaListIterFinished(&main_iter)) {
        int delta_type, main_type;
        long delta_ridx, main_ridx;

        delta_ridx = listMetaIterCur(&delta_iter,&delta_type);
        serverAssert(delta_type == SEGMENT_TYPE_COLD);

        main_ridx = metaListIterCur(&main_iter,&main_type,NULL);
        serverAssert(main_type == SEGMENT_TYPE_HOT);

        if (delta_ridx < main_ridx) {
            listMetaIterNext(&delta_iter);
        } else if (delta_ridx == main_ridx) {
            robj *ele = NULL;
            metaListIterCur(&main_iter,&main_type,&ele);
            cb(main_ridx,ele,pd);
            listMetaIterNext(&delta_iter);
            metaListIterNext(&main_iter);
            selected++;
        } else {
            metaListIterNext(&main_iter);
        }
    }
    listMetaIteratorDeinit(&delta_iter);
    metaListIterDeinit(&main_iter);

    return selected;
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

#ifdef SWAP_LIST_DEBUG
        sds main_dump = listMetaDump(sdsempty(),main->meta);
        sds delta_dump = listMetaDump(sdsempty(),delta);
        serverLog(LL_WARNING,"[list] exclude-%ld: \n  main:%s\n  delta:%s\n",
                ridx,main_dump,delta_dump);
        sdsfree(main_dump);
        sdsfree(delta_dump);
#endif
    }
    listMetaDefrag(main->meta);
    listMetaIteratorDeinit(&delta_iter);

    return excluded;
}

/* List object meta */
objectMeta *createListObjectMeta(uint64_t version, listMeta *list_meta) {
    objectMeta *object_meta = createObjectMeta(OBJ_LIST,version);
    objectMetaSetPtr(object_meta,list_meta);
	return object_meta;
}

#define LIST_META_ENCODED_INITAL_LEN 32
/*  len(# of elements) | num (# of segments) | (ridx,len) ... */

static sds encodeListMeta(listMeta *lm) {
    sds result;

    if (lm == NULL) return NULL;

    result = sdsnewlen(SDS_NOINIT,LIST_META_ENCODED_INITAL_LEN);
    sdsclear(result);

    result = sdscatlen(result,&lm->len,sizeof(lm->len));
    result = sdscatlen(result,&lm->num,sizeof(lm->num));

    for (int i = 0; i < lm->num; i++) {
        segment *seg = lm->segments+i;
        uint8_t segtype = seg->type;
        long ridx = seg->index, len = seg->len;
        result = sdscatlen(result,&segtype,sizeof(segtype));
        result = sdscatlen(result,&ridx,sizeof(ridx));
        result = sdscatlen(result,&len,sizeof(len));
    }

    return result;
}

sds encodeListObjectMeta(struct objectMeta *object_meta) {
    if (object_meta == NULL) return NULL;
    serverAssert(object_meta->object_type == OBJ_LIST);
    return encodeListMeta(objectMetaGetPtr(object_meta));
}

static listMeta *decodeListMeta(const char *extend, size_t extlen) {
    uint8_t segtype;
    long ridx, len;
    listMeta *lm = listMetaCreate();

    if (extlen < sizeof(lm->len)) goto err;
    memcpy(&lm->len,extend,sizeof(lm->len));
    extend += sizeof(lm->len), extlen -= sizeof(lm->len);

    if (extlen < sizeof(lm->num)) goto err;
    memcpy(&lm->num,extend,sizeof(lm->num));
    extend += sizeof(lm->num), extlen -= sizeof(lm->num);

    if (extlen != lm->num * (sizeof(segtype) + sizeof(ridx) + sizeof(len)))
        goto err;
    
    listMetaMakeRoomFor(lm,lm->num);

    for (int i = 0; i < lm->num; i++) {
        segment *seg;

        memcpy(&segtype,extend,sizeof(segtype));
        extend += sizeof(segtype), extlen -= sizeof(segtype);
        memcpy(&ridx,extend,sizeof(ridx));
        extend += sizeof(ridx), extlen -= sizeof(ridx);
        memcpy(&len,extend,sizeof(len));
        extend += sizeof(len), extlen -= sizeof(len);

        seg = lm->segments+i;
        seg->type = segtype;
        seg->index = ridx;
        seg->len = len;
    }

    return lm;

err:
    listMetaFree(lm);
    return NULL;
}

int decodeListObjectMeta(struct objectMeta *object_meta, const char *extend, size_t extlen) {
    serverAssert(object_meta->object_type == OBJ_LIST);
    serverAssert(objectMetaGetPtr(object_meta) == NULL);
    objectMetaSetPtr(object_meta,decodeListMeta(extend,extlen));
    return 0;
}


int listObjectMetaIsHot(objectMeta *object_meta, robj *value) {
    serverAssert(value && object_meta && object_meta->object_type == OBJ_LIST);
    listMeta *lm = objectMetaGetPtr(object_meta);
    if (lm == NULL) {
        return 1;
    } else {
        return listMetaLength(lm,SEGMENT_TYPE_BOTH) ==
            listMetaLength(lm,SEGMENT_TYPE_HOT);
    }
}

void listObjectMetaFree(objectMeta *object_meta) {
    if (object_meta == NULL) return;
    listMetaFree(objectMetaGetPtr(object_meta));
}

void listObjectMetaDup(struct objectMeta *dup_meta, struct objectMeta *object_meta) {
    if (object_meta == NULL) return;
    serverAssert(dup_meta->object_type == OBJ_LIST);
    serverAssert(objectMetaGetPtr(dup_meta) == NULL);
    if (objectMetaGetPtr(object_meta) == NULL) return;
    objectMetaSetPtr(dup_meta,listMetaDup(objectMetaGetPtr(object_meta)));
}

objectMetaType listObjectMetaType = {
    .encodeObjectMeta = encodeListObjectMeta,
    .decodeObjectMeta = decodeListObjectMeta,
    .objectIsHot = listObjectMetaIsHot,
    .free = listObjectMetaFree,
    .duplicate = listObjectMetaDup,
};


/* List swap data */
long ctripListTypeLength(robj *list, objectMeta *object_meta) {
    serverAssert(list || object_meta);
    if (object_meta == NULL) return listTypeLength(list);
    listMeta *lm = objectMetaGetPtr(object_meta);
    return lm->len;
}

static void mockListForDeleteIfCold(swapData *data) {
	if (swapDataIsCold(data)) {
        /* empty list allowed */
		dbAdd(data->db,data->key,createQuicklistObject());
	}
}

static listMeta *swapDataGetListMeta(swapData *data) {
    objectMeta *object_meta = swapDataObjectMeta(data);
    return object_meta ? objectMetaGetPtr(object_meta) : NULL;
}

static void swapDataInitMetaList(swapData *data, metaList *ml) {
    ml->meta = swapDataGetListMeta(data);
    ml->list = data->value;
}

/* unlike hash/set, list elements are either in memlist or rockslist
 * (never both), because otherwise frequently used lpop(rpop) commands
 * have to issue swap to delete pushed elements, which introduces io
 * latency. so if a list is hot, there are no elements in rocksdb. */
int listSwapAna(swapData *data, struct keyRequest *req,
        int *intention, uint32_t *intention_flags, void *datactx_) {
    listDataCtx *datactx = datactx_;
    int cmd_intention = req->cmd_intention;
    uint32_t cmd_intention_flags = req->cmd_intention_flags;

    switch (cmd_intention) {
    case SWAP_NOP:
        *intention = SWAP_NOP;
        *intention_flags = 0;
        break;
    case SWAP_IN:
        if (!swapDataPersisted(data)) {
            /* No need to swap for pure hot key */
            *intention = SWAP_NOP;
            *intention_flags = 0;
        } else if (swapDataIsHot(data)) {
            /* If key is hot, swapAna must be executing in main-thread,
             * we can safely delete meta and turn hot key into pure hot key,
             * which is require for LREM/LINSERT because those command do
             * not maintain list meta. */
            dbDeleteMeta(data->db,data->key);
            *intention = SWAP_NOP;
            *intention_flags = 0;
        } else if (req->l.num_ranges == 0) {
            if (cmd_intention_flags == SWAP_IN_DEL_MOCK_VALUE) {
                datactx->ctx_flag |= BIG_DATA_CTX_FLAG_MOCK_VALUE;
                *intention = SWAP_DEL;
                *intention_flags = SWAP_FIN_DEL_SKIP;
            } else if (cmd_intention_flags == SWAP_IN_META) {
                if (!swapDataIsCold(data)) {
                    *intention = SWAP_NOP;
                    *intention_flags = 0;
                } else {
                    /* LLEN: swap in first element if cold */
                    listMeta *lm = swapDataGetListMeta(data),
                        *swap_meta = listMetaCreate();
                    segment *first_seg = listMetaFirstSegment(lm);
                    listMetaAppendSegment(swap_meta,SEGMENT_TYPE_HOT,
                            first_seg->index,1);
                    datactx->swap_meta = swap_meta;
                    *intention = SWAP_IN;
                    *intention_flags = SWAP_EXEC_IN_DEL;
                }
            } else {
                /* LINSERT/LREM/LPOS, swap in all elements */
                *intention = SWAP_IN;
                *intention_flags = SWAP_EXEC_IN_DEL;
                datactx->swap_meta = NULL;
            }
        } else { /* list range requests */
            listMeta *req_meta, *list_meta, *swap_meta;
            objectMeta *object_meta = swapDataObjectMeta(data);
            long llen = ctripListTypeLength(data->value,object_meta);
            list_meta = swapDataGetListMeta(data);
            serverAssert(list_meta != NULL);
            long ridx_shift = listMetaGetRidxShift(list_meta);

            req_meta = listMetaNormalizeFromRequest(ridx_shift,
                    req->l.num_ranges,req->l.ranges,llen);

            /* req_meta is NULL if range is not valid, in which case swap in
             * all eles (e.g. ltrim removes all eles if range invalid) */
            if (req_meta == NULL) {
                req_meta = listMetaCreate();
                listMetaAppendSegment(req_meta,SEGMENT_TYPE_HOT,ridx_shift,llen);
            }

            if (listMetaLength(req_meta,SEGMENT_TYPE_BOTH) > 0) {
                swap_meta = listMetaCalculateSwapInMeta(list_meta,req_meta);
            } else {
                swap_meta = NULL;
            }

            if (swap_meta && listMetaLength(swap_meta,SEGMENT_TYPE_BOTH) > 0) {
                *intention = SWAP_IN;
                *intention_flags = SWAP_EXEC_IN_DEL;
            } else {
                *intention = SWAP_NOP;
                *intention_flags = 0;
            }
            datactx->swap_meta = swap_meta;
            listMetaFree(req_meta);
        }
        break;
    case SWAP_OUT:
        if (swapDataIsCold(data)) {
            *intention = SWAP_NOP;
            *intention_flags = 0;
        } else {
            listMeta *list_meta;

            if (!swapDataPersisted(data)) {
                /* create new meta if this is a pure hot key */
                listMeta *lm = listMetaCreate();
                listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,
                        listGetInitialRidx(0),
                        listTypeLength(data->value));
                data->new_meta = createListObjectMeta(swapGetAndIncrVersion(),lm);
            }

            list_meta = swapDataGetListMeta(data);
            datactx->swap_meta = listMetaCalculateSwapOutMeta(list_meta);

            *intention = SWAP_OUT;
            *intention_flags = 0;
        }
        break;
    case SWAP_DEL:
        if (!swapDataPersisted(data)) {
            *intention = SWAP_NOP;
            *intention_flags = 0;
        } else if (swapDataIsHot(data)) {
            /* If key is hot, swapAna must be executing in main-thread,
             * we can safely delete meta. */
            dbDeleteMeta(data->db,data->key);
            *intention = SWAP_NOP;
            *intention_flags = 0;
        } else {
            *intention = SWAP_DEL;
            *intention_flags = 0;
        }
        break;
    default:
        break;
    }

    datactx->arg_reqs[0] = req->list_arg_rewrite[0];
    datactx->arg_reqs[1] = req->list_arg_rewrite[1];

    return 0;
}

int listSwapAnaAction(swapData *data, int intention, void *datactx_, int *action) {
    UNUSED(data);
    listDataCtx *datactx = datactx_;
    listMeta *swap_meta = datactx->swap_meta;

    switch (intention) {
        case SWAP_IN:
            if (swap_meta && swap_meta->len > 0) {
                *action = ROCKS_GET;
            } else {/* Swap in entire list(LREM/LINSERT/LPOS...) */
                *action = ROCKS_ITERATE;
            }
            break;
        case SWAP_DEL:
            *action = ROCKS_NOP;
            break;
        case SWAP_OUT:
            *action = ROCKS_PUT;
            break;
        default:
            /* Should not happen .*/
            *action = ROCKS_NOP;
            return SWAP_ERR_DATA_FAIL;
    }

    return 0;
}

static inline sds listEncodeRidx(long ridx) {
    ridx = htonu64(ridx);
    return sdsnewlen(&ridx,sizeof(ridx));
}

static inline sds listEncodeSubkey(redisDb *db, sds key, uint64_t version,
        long ridx) {
    serverAssert(ridx >= 0);
    sds subkey = listEncodeRidx(ridx);
    sds rawkey = rocksEncodeDataKey(db,key,version,subkey);
    sdsfree(subkey);
    return rawkey;
}

static inline long listDecodeRidx(const char *str, size_t len) {
    serverAssert(len == sizeof(long));
    long ridx_be = *(long*)str;
    long ridx = ntohu64(ridx_be);
    return ridx;
}

int listEncodeKeys(swapData *data, int intention, void *datactx_,
        int *numkeys, int **pcfs, sds **prawkeys) {
    listDataCtx *datactx = datactx_;
    listMeta *swap_meta = datactx->swap_meta;
    sds *rawkeys = NULL;
    int *cfs = NULL;
    uint64_t version = swapDataObjectVersion(data);

    serverAssert(SWAP_IN == intention);
    serverAssert(swap_meta && swap_meta->len);
    int neles = 0;
    listMetaIterator iter;

    cfs = zmalloc(sizeof(int)*swap_meta->len);
    rawkeys = zmalloc(sizeof(sds)*swap_meta->len);

    listMetaIteratorInitWithType(&iter,swap_meta,SEGMENT_TYPE_HOT);
    while (!listMetaIterFinished(&iter)) {
        long ridx = listMetaIterCur(&iter,NULL);
        cfs[neles] = DATA_CF;
        rawkeys[neles] = listEncodeSubkey(data->db,data->key->ptr,
                version,ridx);
        neles++;
        listMetaIterNext(&iter);
    }

    *numkeys = neles;
    *pcfs = cfs;
    *prawkeys = rawkeys;

    return 0;
}

static inline sds listEncodeSubval(robj *subval) {
    return rocksEncodeValRdb(subval);
}

typedef struct encodeElementPd {
    int *cfs;
    sds *rawkeys;
    sds *rawvals;
    int capacity;
    int num;
    swapData *data;
} encodeElementPd;

void encodeElement(long ridx, MOVE robj *ele, void *pd_) {
    encodeElementPd *pd = pd_;
    uint64_t version = swapDataObjectVersion(pd->data);
    pd->cfs[pd->num] = DATA_CF;
    pd->rawkeys[pd->num] = listEncodeSubkey(pd->data->db,
            pd->data->key->ptr,version,ridx);
    pd->rawvals[pd->num] = listEncodeSubval(ele);
    pd->num++;
    decrRefCount(ele);
}

int listEncodeData(swapData *data, int intention, void *datactx_,
        int *numkeys, int **pcfs, sds **prawkeys, sds **prawvals) {
    metaList main;
    encodeElementPd *pd;
    listDataCtx *datactx = datactx_;
    listMeta *swap_meta = datactx->swap_meta;
    int capacity = swap_meta->len;

    swapDataInitMetaList(data,&main);

    serverAssert(intention == SWAP_OUT);
    serverAssert(!swapDataIsCold(data));

    pd = zcalloc(sizeof(encodeElementPd));
    pd->num = 0;
    pd->cfs = zmalloc(capacity*sizeof(int));
    pd->rawkeys = zmalloc(capacity*sizeof(sds));
    pd->rawvals = zmalloc(capacity*sizeof(sds));
    pd->capacity = capacity;
    pd->data = data;

    metaListSelect(&main,swap_meta,encodeElement,pd);

    *numkeys = pd->num;
    *pcfs = pd->cfs;
    *prawkeys = pd->rawkeys;
    *prawvals = pd->rawvals;

    zfree(pd);
    return 0;
}

int listEncodeRange(struct swapData *data, int intention, void *datactx_, int *limit,
                    uint32_t *flags, int *pcf, sds *start, sds *end) {
    listDataCtx *datactx = datactx_;
    uint64_t version = swapDataObjectVersion(data);
    serverAssert(SWAP_IN == intention);
    serverAssert(NULL == datactx->swap_meta);

    *flags = 0;
    *pcf = DATA_CF;
    *start = rocksEncodeDataRangeStartKey(data->db,data->key->ptr,version);
    *end = rocksEncodeDataRangeEndKey(data->db,data->key->ptr,version);
    *limit = ROCKS_ITERATE_NO_LIMIT;
    return 0;
}

int listDecodeData(swapData *data, int num, int *cfs, sds *rawkeys,
        sds *rawvals, void **pdecoded) {
    listMeta *meta = listMetaCreate();
    robj *list = createQuicklistObject();
    metaList *delta = metaListBuild(meta,list);
    uint64_t version = swapDataObjectVersion(data);

    serverAssert(num >= 0);
    UNUSED(cfs);

    for (int i = 0; i < num; i++) {
        int dbid;
        long ridx;
        const char *keystr, *subkeystr;
        size_t klen, slen;
        robj *subvalobj;
        uint64_t subkey_version;

        if (rawvals[i] == NULL)
            continue;
        if (rocksDecodeDataKey(rawkeys[i],sdslen(rawkeys[i]),
                &dbid,&keystr,&klen,&subkey_version,&subkeystr,&slen) < 0)
            continue;
        if (!swapDataPersisted(data))
            continue;
        if (slen != sizeof(ridx))
            continue;
        if (version != subkey_version)
            continue;
        ridx = listDecodeRidx(subkeystr,slen);

        subvalobj = rocksDecodeValRdb(rawvals[i]);
        serverAssert(subvalobj->type == OBJ_STRING);
        /* subvalobj might be shared integer, unshared it before
         * add to decoded. */
        subvalobj = unshareStringValue(subvalobj);
        listMetaAppendSegmentWithoutCheck(meta,SEGMENT_TYPE_HOT,ridx,1);
        listTypePush(list,subvalobj,LIST_TAIL);
        decrRefCount(subvalobj);
    }

    *pdecoded = delta;

#ifdef SWAP_LIST_DEBUG
    sds dump = metaListDump(sdsempty(), delta);
    serverLog(LL_WARNING, "[listDecodeData]: %s", dump);
    sdsfree(dump);
#endif

    return 0;
}

void *listCreateOrMergeObject(swapData *data, MOVE void *decoded, void *datactx) {
    void *result;
    UNUSED(datactx);
    metaList *delta = decoded, main;

    if (swapDataIsCold(data) || delta == NULL) {
        /* decoded moved back to swap framework again (result will later be
         * pass as swapIn param). */
        result = delta;
    } else {
        swapDataInitMetaList(data,&main);

#ifdef SWAP_LIST_DEBUG
        sds main_dump = metaListDump(sdsempty(),&main);
        sds delta_dump = metaListDump(sdsempty(),delta);
#endif

        metaListMerge(&main,delta);

#ifdef SWAP_LIST_DEBUG
        sds main_merged_dump = metaListDump(sdsempty(), &main);
        sds delta_merged_dump = metaListDump(sdsempty(), delta);
        serverLog(LL_WARNING,
                "[createOrMerge]:\n main:%s\n delta:%s\n main_merged:%s\n delta_merged:%s\n",
                main_dump, delta_dump, main_merged_dump, delta_merged_dump);
        sdsfree(main_dump), sdsfree(delta_dump), sdsfree(main_merged_dump), sdsfree(delta_merged_dump);
#endif

        metaListDestroy(delta);
        result = NULL;
    }
    return result;
}

int listSwapIn(swapData *data, MOVE void *result_, void *datactx) {
    metaList *result = result_;
    UNUSED(datactx);
    /* hot key no need to swap in, this must be a warm or cold key. */
    serverAssert(swapDataPersisted(data));
    if (swapDataIsCold(data) && result != NULL /* may be empty */) {
        serverAssert(data->cold_meta);
        listMeta *meta = swapDataGetListMeta(data);
        metaList main = {meta,createQuicklistObject()};
        /* memory manage is little bit tricky here:
         * - meta is owned by data->cold_meta, which will be moved to db.meta
         * - list is created and moved to db.dict
         * - contents in result will be swapped or copied to meta & list */
        metaListMerge(&main,result);
        /* mark persistent after data swap in without
         * persistence deleted, or mark non-persistent else */
        main.list->persistent = !data->persistence_deleted;
        /* cold key swapped in result (may be empty). */
        dbAdd(data->db,data->key,main.list);
        /* expire will be swapped in later by swap framework. */
        serverAssert(main.meta == objectMetaGetPtr(data->cold_meta));
        dbAddMeta(data->db,data->key,data->cold_meta);
        data->cold_meta = NULL; /* moved */
        metaListDestroy(result);
    } else {
        if (result) metaListDestroy(result);
        if (data->value) data->value->persistent = !data->persistence_deleted;
    }

    return 0;
}

int listCleanObject(swapData *data, void *datactx_) {
    metaList main;
    listDataCtx *datactx = datactx_;
    if (swapDataIsCold(data)) return 0;
    swapDataInitMetaList(data,&main);


#ifdef SWAP_LIST_DEBUG
    sds main_dump = metaListDump(sdsempty(),&main);
    sds delta_dump = listMetaDump(sdsempty(),datactx->swap_meta);
#endif

    metaListExclude(&main,datactx->swap_meta);

#ifdef SWAP_LIST_DEBUG
    sds main_merged_dump = metaListDump(sdsempty(), &main);
    serverLog(LL_WARNING,
            "[cleanObject]:\n main:%s\n delta:%s\n main_merged:%s\n",
            main_dump, delta_dump, main_merged_dump);
    sdsfree(main_dump), sdsfree(delta_dump), sdsfree(main_merged_dump);
#endif

    return 0;
}

/* subkeys already cleaned by cleanObject(to save cpu usage of main thread),
 * swapout only updates db.dict keyspace, meta (db.meta/db.expire) swapped
 * out by swap framework. */
int listSwapOut(swapData *data, void *datactx, int *totally_out) {
    UNUSED(datactx);
    serverAssert(!swapDataIsCold(data));

    if (listTypeLength(data->value) == 0) {
        /* all elements swapped out, key turnning into cold:
         * - rocks-meta should have already persisted.
         * - object_meta and value will be deleted by dbDelete, expire already
         *   deleted by swap framework. */
        dbDelete(data->db,data->key);
        /* new_meta exists if hot key turns cold directly, in which case
         * new_meta not moved to db.meta nor updated but abandonded. */
        if (data->new_meta) {
            freeObjectMeta(data->new_meta);
            data->new_meta = NULL;
        }
        if (totally_out) *totally_out = 1;
    } else { /* not all elements swapped out. */
        if (data->new_meta) {
            dbAddMeta(data->db,data->key,data->new_meta);
            data->new_meta = NULL; /* moved to db.meta */
            data->value->persistent = 1; /* loss pure hot and persistent data exist. */
        }
        if (totally_out) *totally_out = 0;
    }

    return 0;
}

int listSwapDel(swapData *data, void *datactx_, int del_skip) {
    listDataCtx* datactx = (listDataCtx*)datactx_;
    if (datactx->ctx_flag & BIG_DATA_CTX_FLAG_MOCK_VALUE) {
        mockListForDeleteIfCold(data);
    }
    if (del_skip) {
        if (!swapDataIsCold(data))
            dbDeleteMeta(data->db,data->key);
        return 0;
    } else {
        if (!swapDataIsCold(data))
            /* both value/object_meta/expire are deleted */
            dbDelete(data->db,data->key);
        return 0;
    }
}

/* arg rewrite */
argRewrites *argRewritesCreate() {
    argRewrites *arg_rewrites = zmalloc(sizeof(argRewrites));
    argRewritesReset(arg_rewrites);
    return arg_rewrites;
}

void argRewritesAdd(argRewrites *arg_rewrites, argRewriteRequest arg_req, MOVE robj *orig_arg) {
    serverAssert(arg_rewrites->num < ARG_REWRITES_MAX);
    argRewrite *rewrite = arg_rewrites->rewrites + arg_rewrites->num;
    rewrite->arg_req = arg_req;
    rewrite->orig_arg = orig_arg;
    arg_rewrites->num++;
}

void argRewritesReset(argRewrites *arg_rewrites) {
    memset(arg_rewrites,0,sizeof(argRewrites));
}

void argRewritesFree(argRewrites *arg_rewrites) {
    if (arg_rewrites) zfree(arg_rewrites);
}

void clientArgRewritesRestore(client *c) {
    for (int i = 0; i < c->swap_arg_rewrites->num; i++) {
        argRewrite *rewrite = c->swap_arg_rewrites->rewrites+i;
        int mstate_idx = rewrite->arg_req.mstate_idx, arg_idx = rewrite->arg_req.arg_idx; 
        if (mstate_idx < 0) {
            serverAssert(arg_idx < c->argc);
            decrRefCount(c->argv[arg_idx]);
            c->argv[arg_idx] = rewrite->orig_arg;
        } else {
            serverAssert(mstate_idx < c->mstate.count);
            serverAssert(arg_idx < c->mstate.commands[mstate_idx].argc);
            decrRefCount(c->mstate.commands[mstate_idx].argv[arg_idx]);
            c->mstate.commands[mstate_idx].argv[arg_idx] = rewrite->orig_arg;
        }
    }
    argRewritesReset(c->swap_arg_rewrites);
}

void clientArgRewrite(client *c, argRewriteRequest arg_req, MOVE robj *new_arg) {
    robj *orig_arg;
    if (arg_req.mstate_idx < 0) {
        serverAssert(arg_req.arg_idx < c->argc);
        orig_arg = c->argv[arg_req.arg_idx];
        c->argv[arg_req.arg_idx] = new_arg;
    } else {
        serverAssert(arg_req.mstate_idx < c->mstate.count);
        serverAssert(arg_req.arg_idx < c->mstate.commands[arg_req.mstate_idx].argc);
        orig_arg = c->mstate.commands[arg_req.mstate_idx].argv[arg_req.arg_idx];
        c->mstate.commands[arg_req.mstate_idx].argv[arg_req.arg_idx] = new_arg;
    }
    argRewritesAdd(c->swap_arg_rewrites,arg_req,orig_arg);
}

int listBeforeCall(swapData *data, client *c, void *datactx_) {
    listDataCtx *datactx = datactx_;
    objectMeta *object_meta;
    listMeta *meta;

    object_meta = lookupMeta(data->db,data->key);
    if (object_meta == NULL) return 0;

    serverAssert(object_meta->object_type == OBJ_LIST);
    meta = objectMetaGetPtr(object_meta);

    for (int i = 0; i < 2; i++) {
        argRewriteRequest arg_req = datactx->arg_reqs[i];
        if (arg_req.arg_idx <= 0) continue;
        long long index;
        int ret;

        if (arg_req.mstate_idx < 0) {
            ret = getLongLongFromObject(c->argv[arg_req.arg_idx],&index);
        } else {
            serverAssert(arg_req.mstate_idx < c->mstate.count);
            ret = getLongLongFromObject(c->mstate.commands[arg_req.mstate_idx].argv[arg_req.arg_idx],&index);
        }

        serverAssert(ret == C_OK);
        long midx = listMetaGetMidx(meta,index);
        robj *new_arg = createObject(OBJ_STRING,sdsfromlonglong(midx));
        clientArgRewrite(c,arg_req,new_arg);
    }

    return 0;
}

/* Only free extend fields here, base fields (key/value/object_meta) freed
 * in swapDataFree */
void freeListSwapData(swapData *data_, void *datactx_) {
    UNUSED(data_);
    listDataCtx *datactx = datactx_;
    if (datactx->swap_meta) {
        listMetaFree(datactx->swap_meta);
        datactx->swap_meta = NULL;
    }
    zfree(datactx);
}

int listMetaMergedIsHot(listMeta *main_meta, listMeta *delta_meta) {
    int i = 0, j = 0, ishot = 1;
    long cold_left, cold_right;
    listMeta emptymeta = {0};
    segment *mseg, *dseg;

    if (main_meta == NULL) main_meta = &emptymeta;
    if (delta_meta == NULL) delta_meta = &emptymeta;

    while (i < main_meta->num && ishot) {
        mseg = main_meta->segments+i;

        if (mseg->type == SEGMENT_TYPE_HOT) {
            i++;
            continue;
        }

        /* mseg is continously hot in delta_meta */
        cold_left = mseg->index;
        cold_right = mseg->index + mseg->len;

        while (cold_left < cold_right) {

            if (j >= delta_meta->num) {
                ishot = 0;
                break;
            }

            dseg = delta_meta->segments+j;

            if (dseg->index + dseg->len <= cold_left) {
                /* skip none-overlapping segment, note that we dont care
                 * whether segment type is cold or hot. */
                j++;
                continue;
            }

            if (dseg->type == SEGMENT_TYPE_COLD) {
                ishot = 0;
                break;
            }

            if  (cold_left < dseg->index) {
                ishot = 0;
                break;
            }

            cold_left = dseg->index + dseg->len;
        }

        i++;
    }

    return ishot;
}

int listMergedIsHot(swapData *d, void *result, void *datactx) {
    listMeta *main_meta, *delta_meta;
    metaList *delta = result;
    UNUSED(datactx);
    main_meta = swapDataGetListMeta(d);
    delta_meta = delta ? delta->meta : NULL;
    return listMetaMergedIsHot(main_meta,delta_meta);
}

swapDataType listSwapDataType = {
    .name = "list",
    .swapAna = listSwapAna,
    .swapAnaAction = listSwapAnaAction,
    .encodeKeys = listEncodeKeys,
    .encodeData = listEncodeData,
    .encodeRange = listEncodeRange,
    .decodeData = listDecodeData,
    .swapIn = listSwapIn,
    .swapOut = listSwapOut,
    .swapDel = listSwapDel,
    .createOrMergeObject = listCreateOrMergeObject,
    .cleanObject = listCleanObject,
    .beforeCall = listBeforeCall,
    .free = freeListSwapData,
    .rocksDel = NULL,
    .mergedIsHot = listMergedIsHot,
};

int swapDataSetupList(swapData *d, void **pdatactx) {
    d->type = &listSwapDataType;
    d->omtype = &listObjectMetaType;
    listDataCtx *datactx = zmalloc(sizeof(listDataCtx));
    datactx->swap_meta = NULL;
    datactx->ctx_flag = BIG_DATA_CTX_FLAG_NONE;
    argRewriteRequestInit(datactx->arg_reqs+0);
    argRewriteRequestInit(datactx->arg_reqs+1);
    *pdatactx = datactx;
    return 0;
}

/* List utils */
static inline listMeta *lookupListMeta(redisDb *db, robj *key) {
    objectMeta *object_meta = lookupMeta(db,key);
    if (object_meta == NULL) return NULL;
    serverAssert(object_meta->object_type == OBJ_LIST);
    return objectMetaGetPtr(object_meta);
}

void ctripListTypePush(robj *subject, robj *value, int where, redisDb *db, robj *key) {
    listTypePush(subject,value,where);
    if (server.swap_mode == SWAP_MODE_MEMORY) return; 
    long head = where == LIST_HEAD ? 1 : 0;
    long tail = where == LIST_TAIL ? 1 : 0;
    listMeta *meta = lookupListMeta(db,key); 
    if (meta) listMetaExtend(meta,head,tail);
}

robj *ctripListTypePop(robj *subject, int where, redisDb *db, robj *key) {
    robj *val = listTypePop(subject,where);
    if (server.swap_mode == SWAP_MODE_MEMORY) return val;
    long head = where == LIST_HEAD ? -1 : 0;
    long tail = where == LIST_TAIL ? -1 : 0;
    listMeta *meta = lookupListMeta(db,key); 
    if (meta) listMetaExtend(meta,head,tail);
    return val;
}

void ctripListMetaDelRange(redisDb *db, robj *key, long ltrim, long rtrim) {
    if (server.swap_mode == SWAP_MODE_MEMORY) return;
    listMeta *meta = lookupListMeta(db,key); 
    if (meta) listMetaExtend(meta,-ltrim,-rtrim);
}

/* List rdb save, note that:
 * - hot lists are saved as RDB_TYPE_LIST_QUICKLIST (same as origin redis)
 * - warm/cold list are saved as RDB_TYPE_LIST, which are more suitable
 *   for stream load & save. */

void *listSaveIterCreate(objectMeta *object_meta, robj *list) {
    metaListIterator *iter = zmalloc(sizeof(metaListIterator));
    listMeta *meta = objectMetaGetPtr(object_meta);
    metaList main = {meta, list};
    serverAssert(list != NULL);
    serverAssert((long)listTypeLength(list) == listMetaLength(meta,SEGMENT_TYPE_HOT));
    metaListIterInit(iter,&main);
    return iter;
}

void listSaveIterFree(void *iter) {
    metaListIterDeinit(iter);
    zfree(iter);
}

int listSaveStart(rdbKeySaveData *save, rio *rdb) {
    robj *key = save->key;
    size_t neles = 0;

    /* save header */
    if (rdbSaveKeyHeader(rdb,key,key,RDB_TYPE_LIST,save->expire) == -1)
        return -1;

    /* neles */
    neles = ctripListTypeLength(save->value,save->object_meta);
    if (rdbSaveLen(rdb,neles) == -1)
        return -1;

    return 0;
}

/* save elements in memory untill ridx(not included) */
int listSaveHotElementsUntill(rdbKeySaveData *save, rio *rdb, long ridx) {
    int segtype;
    long curidx;
    metaListIterator *iter = save->iter;

    if (iter == NULL) return 0;

    while (!metaListIterFinished(iter)) {
        curidx = metaListIterCur(iter,&segtype,NULL);
        serverAssert(segtype == SEGMENT_TYPE_HOT);
        if (curidx < ridx) {
            robj *ele;
            metaListIterCur(iter,&segtype,&ele);
            if (rdbSaveStringObject(rdb,ele) == -1) {
                decrRefCount(ele);
                return -1;
            }
            decrRefCount(ele);
            metaListIterNext(iter);
            save->saved++;
        } else {
            break;
        }
    }

    return 0;
}

int listSave(rdbKeySaveData *save, rio *rdb, decodedData *decoded) {
    long ridx;
    robj *key = save->key;
    serverAssert(!sdscmp(decoded->key, key->ptr));

    if (decoded->rdbtype != RDB_TYPE_STRING) {
        /* check failed, skip this key */
        return 0;
    }
    
    /* save elements in prior to current saving ridx in memlist */
    ridx = listDecodeRidx(decoded->subkey,sdslen(decoded->subkey));
    listSaveHotElementsUntill(save,rdb,ridx);

    if (rdbWriteRaw(rdb,(unsigned char*)decoded->rdbraw,
                sdslen(decoded->rdbraw)) == -1) {
        return -1;
    }

    save->saved++;
    return 0;
}

int listSaveEnd(rdbKeySaveData *save, rio *rdb, int save_result) {
    listMeta *meta = objectMetaGetPtr(save->object_meta);
    long meta_len = listMetaLength(meta,SEGMENT_TYPE_BOTH);

    if (save_result != -1) {
        /* save tail hot elements */
        listSaveHotElementsUntill(save,rdb,LIST_MAX_INDEX);
    }

    if (save->saved != meta_len) {
        sds key  = save->key->ptr;
        sds repr = sdscatrepr(sdsempty(), key, sdslen(key));
        serverLog(LL_WARNING,
                "listSave %s: saved(%d) != listmeta.len(%ld)",
                repr, save->saved, meta_len);
        sdsfree(repr);
        return -1;
    }

    return save_result;
}

void listSaveDeinit(rdbKeySaveData *save) {
    if (save->iter) {
        listSaveIterFree(save->iter);
        save->iter = NULL;
    }
}

rdbKeySaveType listSaveType = {
    .save_start = listSaveStart,
    .save = listSave,
    .save_end = listSaveEnd,
    .save_deinit = listSaveDeinit,
};

int listSaveInit(rdbKeySaveData *save, uint64_t version, const char *extend, size_t extlen) {
    int retval = 0;
    save->type = &listSaveType;
    save->omtype = &listObjectMetaType;
    if (extend) { /* cold */
        serverAssert(save->object_meta == NULL && save->value == NULL);
        retval = buildObjectMeta(OBJ_LIST,version,extend,extlen,&save->object_meta);
    } else { /* warm */
        serverAssert(save->object_meta && save->value);
        save->iter = listSaveIterCreate(save->object_meta,save->value);
    }
    return retval;
}

static sds listLoadEncodeObjectMetaExtend(size_t llen) {
    sds extend = NULL;
    listMeta *meta = listMetaCreate();
    listMetaAppendSegment(meta,SEGMENT_TYPE_COLD,listGetInitialRidx(0),llen);
    extend = encodeListMeta(meta);
    listMetaFree(meta);
    return extend;
}

/* List rdb load */
void listLoadStartWithValue(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    size_t llen;
    sds extend;

    load->value = rdbLoadObject(load->rdbtype,rdb,load->key,error);
    if (load->value == NULL) return;

    if (load->value->type != OBJ_LIST) {
        serverLog(LL_WARNING,"Load rdb with rdbtype(%d) got (%d)",
                load->rdbtype, load->value->type);
        *error = RDB_LOAD_ERR_OTHER;
        return;
    }

    if ((llen = listTypeLength(load->value)) == 0) {
        *error = RDB_LOAD_ERR_EMPTY_KEY;
        return;
    }

    /* list supports only quicklist encoding now, convert ziplist to
     * quicklist before iterating. */
    if (load->value->encoding == OBJ_ENCODING_ZIPLIST)
        listTypeConvert(load->value,OBJ_ENCODING_QUICKLIST);

    load->iter = listTypeInitIterator(load->value,0,LIST_TAIL);
    load->total_fields = llen;

    extend = listLoadEncodeObjectMetaExtend(llen);

    *cf = META_CF;
    *rawkey = rocksEncodeMetaKey(load->db,load->key);
    *rawval = rocksEncodeMetaVal(load->object_type,load->expire,load->version,extend);

    sdsfree(extend);
}

void listLoadStartList(struct rdbKeyLoadData *load, rio *rdb, int *cf,
                    sds *rawkey, sds *rawval, int *error) {
    int isencode;
    unsigned long long llen;
    sds header, extend = NULL;

    header = rdbVerbatimNew((unsigned char)load->rdbtype);

    /* nfield */
    if (rdbLoadLenVerbatim(rdb,&header,&isencode,&llen)) {
        sdsfree(header);
        *error = RDB_LOAD_ERR_OTHER;
        return;
    }

    load->total_fields = llen;

    extend = listLoadEncodeObjectMetaExtend(llen);

    *cf = META_CF;
    *rawkey = rocksEncodeMetaKey(load->db,load->key);
    *rawval = rocksEncodeMetaVal(load->object_type,load->expire,load->version,extend);
    *error = 0;

    sdsfree(extend);
    sdsfree(header);
}

void listLoadStart(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    switch (load->rdbtype) {
    case RDB_TYPE_LIST_ZIPLIST:
        listLoadStartWithValue(load,rdb,cf,rawkey,rawval,error);
        break;
    case RDB_TYPE_LIST_QUICKLIST:
        listLoadStartWithValue(load,rdb,cf,rawkey,rawval,error);
        break;
    case RDB_TYPE_LIST:
        listLoadStartList(load,rdb,cf,rawkey,rawval,error);
        break;
    default:
        break;
    }
}

int listLoadWithValue(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    listTypeEntry entry;
    robj *ele;
    long ridx;
    
    UNUSED(rdb);

    serverAssert(listTypeNext(load->iter,&entry));
    ele = listTypeGet(&entry);

    ridx = listGetInitialRidx(load->loaded_fields);

    *cf = DATA_CF;
    *rawkey = listEncodeSubkey(load->db,load->key,load->version,ridx);
    *rawval = listEncodeSubval(ele);
    *error = 0;

    decrRefCount(ele);
    load->loaded_fields++;
    return load->loaded_fields < load->total_fields;
}

int listLoadList(struct rdbKeyLoadData *load, rio *rdb, int *cf, sds *rawkey,
        sds *rawval, int *error) {
    sds rdbval;
    long ridx;

    *error = RDB_LOAD_ERR_OTHER;

    rdbval = rdbVerbatimNew(RDB_TYPE_STRING);
    if (rdbLoadStringVerbatim(rdb,&rdbval)) {
        sdsfree(rdbval);
        return 0;
    }

    ridx = listGetInitialRidx(load->loaded_fields);

    *cf = DATA_CF;
    *rawkey = listEncodeSubkey(load->db,load->key,load->version,ridx);
    *rawval = rdbval;
    *error = 0;

    load->loaded_fields++;
    return load->loaded_fields < load->total_fields;
}

int listLoad(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    int retval;

    switch (load->rdbtype) {
    case RDB_TYPE_LIST:
        retval = listLoadList(load,rdb,cf,rawkey,rawval,error);
        break;
    case RDB_TYPE_LIST_QUICKLIST:
    case RDB_TYPE_LIST_ZIPLIST:
        retval = listLoadWithValue(load,rdb,cf,rawkey,rawval,error);
        break;
    default:
        retval = RDB_LOAD_ERR_OTHER;
        break;
    }

    return retval;
}

void listLoadDeinit(struct rdbKeyLoadData *load) {
    if (load->iter) {
        listTypeReleaseIterator(load->iter);
        load->iter = NULL;
    }

    if (load->value) {
        decrRefCount(load->value);
        load->value = NULL;
    }
}

rdbKeyLoadType listLoadType = {
    .load_start = listLoadStart,
    .load = listLoad,
    .load_end = NULL,
    .load_deinit = listLoadDeinit,
};

void listLoadInit(rdbKeyLoadData *load) {
    load->type = &listLoadType;
    load->omtype = &listObjectMetaType;
    load->object_type = OBJ_LIST;
}

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

void selectElements(long ridx, robj *ele, void *pd) {
    UNUSED(ridx),UNUSED(ele),UNUSED(pd);
    robj *list = pd;
    listTypePush(list,ele,LIST_TAIL);
    decrRefCount(ele);
}

void initServerConfig(void);
int swapListMetaTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;

    TEST("list: init") {
        initTestRedisServer();
    }

    TEST("list-meta: basics & iterator") {
        listMeta *lm = listMetaCreate();
        test_assert(listMetaIsValid(lm,LIST_META_STRICT_CONTINOUS|LIST_META_STRICT_NOEMPTY));
        test_assert(listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,0,2) == 0);
        test_assert(listMetaIsValid(lm,LIST_META_STRICT_CONTINOUS|LIST_META_STRICT_NOEMPTY));
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
        test_assert(listMetaIsValid(lm,LIST_META_STRICT_CONTINOUS|LIST_META_STRICT_NOEMPTY));
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

        /* 10~11(HOT)|12(HOT)|12~13(HOT)|14~15(COLD)|20~21(COLD)|22~23(COLD) */
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
        qm = listMetaNormalizeFromRequest(0,2,ltrim1,4);
        test_assert(qm == NULL);


        range ltrim2[2] = {{0,1},{-2,-1}};
        qm = listMetaNormalizeFromRequest(0,2,ltrim2,4);
        test_assert(listMetaIsValid(qm,0));
        test_assert(qm->num == 2 && qm->len == 4);
        test_assert(qm->segments[0].index == 0 && qm->segments[0].len == 2); 
        test_assert(qm->segments[1].index == 2 && qm->segments[0].len == 2); 
        listMetaFree(qm);

        range within_range[2] = { {0,1},{-5,-4}};
        qm = listMetaNormalizeFromRequest(0,2,within_range,4);
        test_assert(qm->num == 2 && qm->len == 3);
        test_assert(qm->segments[0].index == 0 && qm->segments[0].len == 2); 
        test_assert(qm->segments[1].index == 0 && qm->segments[1].len == 1); 
        listMetaFree(qm);

        range exceed_range[2] = { {0,1},{-5,-5}};
        qm = listMetaNormalizeFromRequest(0,2,exceed_range,4);
        test_assert(qm == NULL);
    }

    TEST("list-meta: search overlaps") {
        segment seg = {SEGMENT_TYPE_HOT,0,0};
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

    TEST("list-meta: calculate swap in meta") {
        listMeta *lm = listMetaCreate(), *qm, *sm;
        /* hot */
        listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,0,4);
        range ltrim[2] = { {0,0}, {-1,-1} };
        qm = listMetaNormalizeFromRequest(0,2,ltrim,4);
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
        qm = listMetaNormalizeFromRequest(0,1,req1,60);
        sm = listMetaCalculateSwapInMeta(lm,qm);
        test_assert(sm->num == 2 && sm->len == 15);
        test_assert(sm->segments[0].index == 15 && sm->segments[0].len == 5);
        test_assert(sm->segments[1].index == 30 && sm->segments[1].len == 10);
        listMetaFree(qm), listMetaFree(sm);

        range req2[5] = { {5,14}, {15,24}, {25,29}, {30,54}, {55,59}};
        qm = listMetaNormalizeFromRequest(0,5,req2,60);
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

    TEST("list-meta: objectMeta encode/decode") {
        listMeta *lm = listMetaCreate();
        listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,0,2);
        listMetaAppendSegment(lm,SEGMENT_TYPE_COLD,2,2);
        listMetaAppendSegment(lm,SEGMENT_TYPE_HOT,4,2);
        objectMeta *object_meta = createListObjectMeta(0,lm);
        sds extend = objectMetaEncode(object_meta);
        objectMeta *decoded_meta = createObjectMeta(OBJ_LIST,0);
        test_assert(objectMetaDecode(decoded_meta,extend,sdslen(extend)) == 0);
        listMeta *decoded_lm = objectMetaGetPtr(decoded_meta);
        test_assert(listMetaLength(decoded_lm,SEGMENT_TYPE_BOTH) == 6);
        test_assert(listMetaLength(decoded_lm,SEGMENT_TYPE_COLD) == 2);
        test_assert(decoded_lm->num == 3);
        segment *seg;
        seg = decoded_lm->segments+0;
        test_assert(seg->type == SEGMENT_TYPE_HOT && seg->index == 0 && seg->len == 2);
        seg = decoded_lm->segments+1;
        test_assert(seg->type == SEGMENT_TYPE_COLD && seg->index == 2 && seg->len == 2);
        seg = decoded_lm->segments+2;
        test_assert(seg->type == SEGMENT_TYPE_HOT && seg->index == 4 && seg->len == 2);
        freeObjectMeta(object_meta);
        freeObjectMeta(decoded_meta);
        sdsfree(extend);
    }

    TEST("list-meta: mergedIsHot") {
        listMeta *main_meta, *delta_meta;

        main_meta = listMetaCreate();
        listMetaAppendSegment(main_meta,SEGMENT_TYPE_HOT,0,2);
        listMetaAppendSegment(main_meta,SEGMENT_TYPE_COLD,2,2);
        listMetaAppendSegment(main_meta,SEGMENT_TYPE_HOT,4,2);

        delta_meta = listMetaCreate();
        listMetaAppendSegment(delta_meta,SEGMENT_TYPE_HOT,0,2);
        test_assert(!listMetaMergedIsHot(main_meta, delta_meta));
        listMetaFree(delta_meta);

        delta_meta = listMetaCreate();
        listMetaAppendSegment(delta_meta,SEGMENT_TYPE_HOT,1,2);
        test_assert(!listMetaMergedIsHot(main_meta, delta_meta));
        listMetaFree(delta_meta);

        delta_meta = listMetaCreate();
        listMetaAppendSegment(delta_meta,SEGMENT_TYPE_HOT,2,2);
        test_assert(listMetaMergedIsHot(main_meta, delta_meta));
        listMetaFree(delta_meta);

        delta_meta = listMetaCreate();
        listMetaAppendSegment(delta_meta,SEGMENT_TYPE_HOT,3,2);
        test_assert(!listMetaMergedIsHot(main_meta, delta_meta));
        listMetaFree(delta_meta);

        delta_meta = listMetaCreate();
        listMetaAppendSegment(delta_meta,SEGMENT_TYPE_HOT,2,1);
        test_assert(!listMetaMergedIsHot(main_meta, delta_meta));
        listMetaFree(delta_meta);

        delta_meta = listMetaCreate();
        listMetaAppendSegment(delta_meta,SEGMENT_TYPE_HOT,0,4);
        test_assert(listMetaMergedIsHot(main_meta, delta_meta));
        listMetaFree(delta_meta);

        delta_meta = listMetaCreate();
        listMetaAppendSegment(delta_meta,SEGMENT_TYPE_HOT,0,6);
        test_assert(listMetaMergedIsHot(main_meta, delta_meta));
        listMetaFree(delta_meta);

        delta_meta = listMetaCreate();
        listMetaAppendSegment(delta_meta,SEGMENT_TYPE_HOT,0,1);
        listMetaAppendSegment(delta_meta,SEGMENT_TYPE_HOT,2,1);
        listMetaAppendSegment(delta_meta,SEGMENT_TYPE_HOT,3,2);
        test_assert(listMetaMergedIsHot(main_meta, delta_meta));
        listMetaFree(delta_meta);

        delta_meta = listMetaCreate();
        listMetaAppendSegment(delta_meta,SEGMENT_TYPE_HOT,0,1);
        listMetaAppendSegment(delta_meta,SEGMENT_TYPE_HOT,3,2);
        test_assert(!listMetaMergedIsHot(main_meta, delta_meta));
        listMetaFree(delta_meta);

        listMetaFree(main_meta);
    }

    TEST("meta-list: merge") {
        listMeta *meta = listMetaCreate();
        robj *list = createQuicklistObject();
        metaList *main = metaListBuild(meta,list);

        metaListPush6Seg(main);
        
        /* skip if overlaps with main hot */
        range req1[1] = { {5,5} };
        listMeta *meta1 = listMetaNormalizeFromRequest(0,1,req1,60);
        robj *list1 = createQuicklistObject();
        metaList *delta1 = metaListBuild(meta1,list1);
        metaListPopulateList(delta1);
        test_assert(metaListMerge(main,delta1) == 0);
        test_assert(meta->len == 60 && meta->num == 6 && listTypeLength(list) == 30);
        metaListDestroy(delta1);

        /* merge with hot */
        range req2[1] = { {10,11} };
        listMeta *meta2 = listMetaNormalizeFromRequest(0,1,req2,60);
        robj *list2 = createQuicklistObject();
        metaList *delta2 = metaListBuild(meta2,list2);
        metaListPopulateList(delta2);
        test_assert(metaListMerge(main,delta2) == 2);
        test_assert(meta->len == 60 && meta->num == 6 && listTypeLength(list) == 32);
        metaListDestroy(delta2);

        /* merge and split */
        range req3[1] = { {14,15}, };
        listMeta *meta3 = listMetaNormalizeFromRequest(0,1,req3,60);
        robj *list3 = createQuicklistObject();
        metaList *delta3 = metaListBuild(meta3,list3);
        metaListPopulateList(delta3);
        test_assert(metaListMerge(main,delta3) == 2);
        test_assert(meta->len == 60 && meta->num == 8 && listTypeLength(list) == 34);
        metaListDestroy(delta3);

        /* complex overlap */
        range req4[3] = { {4,4}, {5,44}, {48,57} };
        listMeta *meta4 = listMetaNormalizeFromRequest(0,3,req4,60);
        robj *list4 = createQuicklistObject();
        metaList *delta4 = metaListBuild(meta4,list4);
        metaListPopulateList(delta4);
        test_assert(metaListMerge(main,delta4) == 7);
        test_assert(meta->len == 60 && meta->num == 2 && listTypeLength(list) == 58);
        metaListDestroy(delta4);

        /* edge case */
        range req5[1] = { {1,1}};
        listMeta *mainmeta5 = listMetaCreate(), *meta5 = listMetaNormalizeFromRequest(0,1,req5,3);
        listMetaAppendSegment(mainmeta5,SEGMENT_TYPE_HOT,0,1);
        listMetaAppendSegment(mainmeta5,SEGMENT_TYPE_COLD,1,2);
        robj *mainlist5 = createQuicklistObject(), *list5 = createQuicklistObject();
        metaList *main5 = metaListBuild(mainmeta5,mainlist5), *delta5 = metaListBuild(meta5,list5);
        metaListPopulateList(main5), metaListPopulateList(delta5);
        test_assert(metaListMerge(main5,delta5) == 1);
        test_assert(mainmeta5->len == 3 && mainmeta5->num == 2 && listTypeLength(mainlist5) == 2);
        metaListDestroy(delta5), metaListDestroy(main5);

        metaListDestroy(main);
    }

    TEST("meta-list: exclude") {
        listMeta *meta = listMetaCreate();
        robj *list = createQuicklistObject();
        metaList *main = metaListBuild(meta,list);

        metaListPush6Seg(main);
        
        /* skip if overlaps with main cold */
        range req1[1] = { {10,11} };
        listMeta *meta1 = listMetaNormalizeFromRequest(0,1,req1,60);
        turnListMeta2Type(meta1,SEGMENT_TYPE_COLD);
        test_assert(metaListExclude(main,meta1) == 0);
        test_assert(meta->len == 60 && meta->num == 6 && listTypeLength(list) == 30);
        listMetaFree(meta1);

        /* exclude cold segment */
        range req2[1] = { {0,1} };
        listMeta *meta2 = listMetaNormalizeFromRequest(0,1,req2,60);
        turnListMeta2Type(meta2,SEGMENT_TYPE_COLD);
        test_assert(metaListExclude(main,meta2) == 2);
        test_assert(meta->len == 60 && meta->num == 7 && listTypeLength(list) == 28);
        listMetaFree(meta2);

        /* exclude and split */
        range req3[1] = { {25,26} };
        listMeta *meta3 = listMetaNormalizeFromRequest(0,1,req3,60);
        turnListMeta2Type(meta3,SEGMENT_TYPE_COLD);
        test_assert(metaListExclude(main,meta3) == 2);
        test_assert(meta->len == 60 && meta->num == 9 && listTypeLength(list) == 26);
        listMetaFree(meta3);

        /* complex */
        range req4[3] = { {5,14}, {15,44}, {50,52} };
        listMeta *meta4 = listMetaNormalizeFromRequest(0,3,req4,60);
        turnListMeta2Type(meta4,SEGMENT_TYPE_COLD);
        test_assert(metaListExclude(main,meta4) == 18);
        test_assert(meta->len == 60 && meta->num == 5 && listTypeLength(list) == 8);
        listMetaFree(meta4);
        metaListDestroy(main);
    }

    TEST("meta-list: select") {
        listMeta *meta = listMetaCreate();
        robj *list = createQuicklistObject();
        metaList *main = metaListBuild(meta,list);
        robj *selected;

        metaListPush6Seg(main);
        selected = createQuicklistObject();

        /* skip if overlaps with main cold */
        range req1[1] = { {10,11} };
        listMeta *meta1 = listMetaNormalizeFromRequest(0,1,req1,60);
        turnListMeta2Type(meta1,SEGMENT_TYPE_COLD);
        test_assert(metaListSelect(main,meta1,selectElements,selected) == 0);
        test_assert(listTypeLength(selected) == 0);
        listMetaFree(meta1);

        /* select cold segment */
        range req2[1] = { {0,1} };
        listMeta *meta2 = listMetaNormalizeFromRequest(0,1,req2,60);
        turnListMeta2Type(meta2,SEGMENT_TYPE_COLD);
        test_assert(metaListSelect(main,meta2,selectElements,selected) == 2);
        test_assert(listTypeLength(selected) == 2);
        listMetaFree(meta2);

        /* exclude and split */
        range req3[1] = { {25,26} };
        listMeta *meta3 = listMetaNormalizeFromRequest(0,1,req3,60);
        turnListMeta2Type(meta3,SEGMENT_TYPE_COLD);
        test_assert(metaListSelect(main,meta3,selectElements,selected) == 2);
        test_assert(listTypeLength(selected) == 4);
        listMetaFree(meta3);

        /* complex */
        decrRefCount(selected), selected = createQuicklistObject();
        range req4[3] = { {5,14}, {15,44}, {50,52} };
        listMeta *meta4 = listMetaNormalizeFromRequest(0,3,req4,60);
        turnListMeta2Type(meta4,SEGMENT_TYPE_COLD);
        test_assert(metaListSelect(main,meta4,selectElements,selected) == 20);
        test_assert(listTypeLength(selected) == 20);
        listMetaFree(meta4);
        metaListDestroy(main);
        decrRefCount(selected);
    }

    return error;
}

#define cleanListTestData() do {                        \
    /* clean previous state */                          \
    if (puredata) swapDataFree(puredata,puredatactx);   \
    if (hotdata) swapDataFree(hotdata,hotdatactx);      \
    if (warmdata) swapDataFree(warmdata,warmdatactx);   \
    if (colddata) swapDataFree(colddata,colddatactx);   \
    dbDelete(db,purekey), dbDelete(db,hotkey), dbDelete(db,warmkey), dbDelete(db,coldkey); \
} while (0)

#define setListTestData() do {              \
    /* create new state */                  \
    pure = createQuicklistObject();         \
    hot = createQuicklistObject();          \
    warm = createQuicklistObject();         \
    listTypePush(pure,ele1,LIST_TAIL), listTypePush(pure,ele2,LIST_TAIL), listTypePush(pure,ele3,LIST_TAIL);\
    listTypePush(hot,ele1,LIST_TAIL), listTypePush(hot,ele2,LIST_TAIL), listTypePush(hot,ele3,LIST_TAIL);   \
    listTypePush(warm,ele1,LIST_TAIL);      \
    hotlm = listMetaCreate();               \
    warmlm = listMetaCreate();              \
    coldlm = listMetaCreate();              \
    listMetaAppendSegment(hotlm,SEGMENT_TYPE_HOT,0,3);      \
    listMetaAppendSegment(warmlm,SEGMENT_TYPE_HOT,0,1);     \
    listMetaAppendSegment(warmlm,SEGMENT_TYPE_COLD,1,2);    \
    listMetaAppendSegment(coldlm,SEGMENT_TYPE_COLD,0,3);    \
    hotmeta = createListObjectMeta(0,hotlm);      \
    warmmeta = createListObjectMeta(0,warmlm);    \
    coldmeta = createListObjectMeta(0,coldlm);    \
    puredata = createSwapData(db,purekey,pure); \
    hotdata = createSwapData(db,hotkey,hot);    \
    warmdata = createSwapData(db,warmkey,warm); \
    colddata = createSwapData(db,coldkey,NULL); \
    swapDataSetupMeta(puredata,OBJ_LIST,-1,&puredatactx);   \
    swapDataSetupMeta(hotdata,OBJ_LIST,-1,&hotdatactx), swapDataSetObjectMeta(hotdata,hotmeta);         \
    swapDataSetupMeta(warmdata,OBJ_LIST,-1,&warmdatactx), swapDataSetObjectMeta(warmdata,warmmeta);     \
    swapDataSetupMeta(colddata,OBJ_LIST,-1,&colddatactx), swapDataSetColdObjectMeta(colddata,coldmeta); \
    dbAdd(db,purekey,pure), dbAdd(db,hotkey,hot), dbAdd(db,warmkey,warm); \
    dbAddMeta(db,hotkey,hotmeta), dbAddMeta(db,warmkey,warmmeta); \
} while (0)

#define resetListTestData() do {            \
    cleanListTestData();                    \
    setListTestData();                      \
} while (0)

void swapDataSetColdObjectMeta(swapData *d, MOVE objectMeta *object_meta);

sds rdbEncodeStringObject(robj *o) {
    rio rdb;
    serverAssert(o->type == OBJ_STRING);
    rioInitWithBuffer(&rdb,sdsempty());
    rdbSaveObject(&rdb,o,NULL);
    return rdb.io.buffer.ptr;
}

void rewriteResetClientCommandCString(client *c, int argc, ...);

int swapListDataTest(int argc, char *argv[], int accurate) {
    UNUSED(argc), UNUSED(argv), UNUSED(accurate);
    int error = 0;

    redisDb *db;
    robj *ele1, *ele2, *ele3;
    robj *purekey, *hotkey, *warmkey, *coldkey;
    robj *pure, *hot, *warm;
    listMeta *hotlm, *warmlm, *coldlm;
    objectMeta *hotmeta, *warmmeta, *coldmeta;
    swapData *puredata = NULL, *hotdata = NULL, *warmdata = NULL, *colddata = NULL;
    void *puredatactx = NULL, *hotdatactx = NULL, *warmdatactx = NULL, *colddatactx = NULL;
    long long NOW = 1661657836000;

    TEST("list-data: init") {
        initServerConfig();
        ACLInit();
        server.hz = 10;
        initTestRedisServer();
        db = server.db;
        server.swap_evict_step_max_memory = 1*1024*1024;
        server.swap_evict_step_max_subkeys = 1024;

        ele1 = createStringObject("ele1",4);
        ele2 = createStringObject("ele2",4);
        ele3 = createStringObject("ele3",4);
        purekey = createStringObject("pure",3);
        hotkey = createStringObject("hot",3);
        warmkey = createStringObject("warm",4);
        coldkey = createStringObject("cold",4);

        resetListTestData();
    }

    TEST("list-data: swapAna") {
        int intention;
        uint32_t intention_flags;
        keyRequest kr[1];
        range *full = zmalloc(sizeof(range));
        full->start = 0, full->end = 3;
        kr->level = REQUEST_LEVEL_KEY, kr->dbid = 0;
        /* nop: pure/hot/in.meta warm/... */
        kr->cmd_intention = SWAP_IN, kr->cmd_intention_flags = 0, kr->key = purekey, kr->l.num_ranges = 1, kr->l.ranges = full;
        swapDataAna(puredata,kr,&intention,&intention_flags,puredatactx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        kr->cmd_intention = SWAP_IN, kr->cmd_intention_flags = 0, kr->key = hotkey, kr->l.num_ranges = 1, kr->l.ranges = full;
        swapDataAna(hotdata,kr,&intention,&intention_flags,hotdatactx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        kr->cmd_intention = SWAP_IN, kr->cmd_intention_flags = SWAP_IN_META, kr->key = warmkey, kr->l.num_ranges = 0, kr->l.ranges = NULL;
        swapDataAna(warmdata,kr,&intention,&intention_flags,warmdatactx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        /* in: in warm/in.meta cold/... */
        kr->cmd_intention = SWAP_IN, kr->cmd_intention_flags = 0, kr->key = warmkey, kr->l.num_ranges = 0, kr->l.ranges = NULL;
        swapDataAna(warmdata,kr,&intention,&intention_flags,warmdatactx);
        test_assert(intention == SWAP_IN && intention_flags == SWAP_IN_DEL);
        listDataCtx *warmctx = warmdatactx;
        test_assert(warmctx->swap_meta == NULL/*swap whole key*/);
        kr->cmd_intention = SWAP_IN, kr->cmd_intention_flags = SWAP_IN_META, kr->key = coldkey, kr->l.num_ranges = 0, kr->l.ranges = NULL;
        swapDataAna(colddata,kr,&intention,&intention_flags,colddatactx);
        test_assert(intention == SWAP_IN && intention_flags == SWAP_IN_DEL);
        listDataCtx *coldctx = colddatactx;
        test_assert(coldctx->swap_meta->len == 1 && coldctx->swap_meta->segments[0].len == 1);
        /* out: out by small steps  */
        kr->cmd_intention = SWAP_OUT, kr->cmd_intention_flags = 0, kr->key = purekey, kr->l.num_ranges = 0, kr->l.ranges = NULL;
        swapDataAna(puredata,kr,&intention,&intention_flags,puredatactx);
        test_assert(intention == SWAP_OUT && intention_flags == 0);
        listDataCtx *purectx = puredatactx;
        test_assert(purectx->swap_meta->len == 3 && purectx->swap_meta->segments[0].len == 3);
        /* del: in.mock cold/del cold */
        kr->cmd_intention = SWAP_IN, kr->cmd_intention_flags = SWAP_IN_DEL_MOCK_VALUE, kr->key = coldkey, kr->l.num_ranges = 0, kr->l.ranges = NULL;
        swapDataAna(colddata,kr,&intention,&intention_flags,colddatactx);
        test_assert(intention == SWAP_DEL && intention_flags == SWAP_FIN_DEL_SKIP);
        kr->cmd_intention = SWAP_DEL, kr->cmd_intention_flags = 0, kr->key = coldkey, kr->l.num_ranges = 0, kr->l.ranges = NULL;
        swapDataAna(colddata,kr,&intention,&intention_flags,colddatactx);
        test_assert(intention == SWAP_DEL && intention_flags == 0);

        zfree(full);
        resetListTestData();
    }

    TEST("list-data: encode/decode") {
        int action, numkeys, *cfs;
        sds *rawkeys, *rawvals,
            rawkey0 = listEncodeSubkey(db,hotkey->ptr,0,0),
            rawval0 = listEncodeSubval(ele1);
        metaList *decoded;
        listDataCtx *hotctx = hotdatactx;
        hotctx->swap_meta = listMetaCreate();
        listMetaAppendSegment(hotctx->swap_meta,SEGMENT_TYPE_COLD,0,3);

        listSwapAnaAction(hotdata,SWAP_OUT,hotdatactx,&action);
        listEncodeData(hotdata,SWAP_OUT,hotdatactx,&numkeys,&cfs,&rawkeys,&rawvals);
        test_assert(action == ROCKS_PUT && numkeys == 3 && cfs[0] == DATA_CF);
        test_assert(!sdscmp(rawkeys[0],rawkey0) && !sdscmp(rawvals[0],rawval0));

        listDecodeData(hotdata,numkeys,cfs,rawkeys,rawvals,(void**)&decoded);
        test_assert(listTypeLength(decoded->list) == 3);
        test_assert(listMetaLength(decoded->meta,SEGMENT_TYPE_BOTH) == 3);
        test_assert(listMetaLength(decoded->meta,SEGMENT_TYPE_HOT) == 3);

        sdsfree(rawkey0), sdsfree(rawval0);
        for (int i = 0; i < numkeys; i++) {
            sdsfree(rawkeys[i]), sdsfree(rawvals[i]);
        }
        zfree(cfs), zfree(rawkeys), zfree(rawvals);
        metaListDestroy(decoded);

        resetListTestData();
    }

    TEST("list-data: swapin/swapout case-1") {
        /* pure => warm => cold */
        listMeta *swap_meta;
        objectMeta *object_meta;
        robj *value;
        listDataCtx *purectx = puredatactx;

        listMeta *purelm = listMetaCreate();
        listMetaAppendSegment(purelm,SEGMENT_TYPE_HOT,0,3);
        objectMeta *puremeta = createListObjectMeta(0,purelm);
        swapDataSetNewObjectMeta(puredata,puremeta);  
        swap_meta = listMetaCreate();
        listMetaAppendSegment(swap_meta,SEGMENT_TYPE_COLD,1,2);
        purectx->swap_meta = swap_meta;
        listCleanObject(puredata,puredatactx);
        listSwapOut(puredata,puredatactx,NULL);
        object_meta = lookupMeta(db,purekey);
        test_assert(object_meta != NULL);
        test_assert(listMetaLength(objectMetaGetPtr(object_meta),SEGMENT_TYPE_BOTH) == 3);
        test_assert(listMetaLength(objectMetaGetPtr(object_meta),SEGMENT_TYPE_COLD) == 2);
        listMetaFree(swap_meta);
        /* after swap out, puremeta moved from swapdata to db.meta, so we
         * need to set object_meta for puredata(now warm actually) again. */
        puremeta = lookupMeta(db,purekey);
        swapDataSetObjectMeta(puredata,puremeta);
        swap_meta = listMetaCreate();
        listMetaAppendSegment(swap_meta,SEGMENT_TYPE_COLD,0,3/*exceeds range*/);
        purectx->swap_meta = swap_meta;
        listCleanObject(puredata,puredatactx);
        listSwapOut(puredata,puredatactx,NULL);
        object_meta = lookupMeta(db,purekey);
        test_assert(object_meta == NULL);
        value = lookupKey(db,purekey,LOOKUP_NOTOUCH);
        test_assert(value == NULL);

        /* cold => warm => hot */
        listMeta *delta1_meta = listMetaCreate();
        listMetaAppendSegment(delta1_meta,SEGMENT_TYPE_HOT,1,1);
        metaList *delta1 = metaListBuild(delta1_meta,createQuicklistObject());
        metaListPopulateList(delta1);
        listCreateOrMergeObject(colddata,delta1,colddatactx);
        listSwapIn(colddata,delta1/*moved*/,colddatactx);
        value = lookupKey(db,coldkey,LOOKUP_NOTOUCH);
        test_assert(value != NULL && listTypeLength(value) == 1);
        object_meta = lookupMeta(db,coldkey);
        test_assert(object_meta != NULL && 
                listMetaLength(objectMetaGetPtr(object_meta),SEGMENT_TYPE_BOTH) == 3 &&
                listMetaLength(objectMetaGetPtr(object_meta),SEGMENT_TYPE_HOT) == 1);

        /* after swap in cold_meta will be moved to db.meta, reset to swapin again. */
        swapDataSetObjectMeta(colddata,object_meta);
        incrRefCount(value);
        colddata->value = value;
        listMeta *delta2_meta = listMetaCreate();
        listMetaAppendSegment(delta2_meta,SEGMENT_TYPE_HOT,0,3);
        metaList *delta2 = metaListBuild(delta2_meta,createQuicklistObject());
        metaListPopulateList(delta2);
        listCreateOrMergeObject(colddata,delta2,colddatactx);
        listSwapIn(colddata,NULL/*merged*/,colddatactx);
        test_assert(listTypeLength(value) == 3);
        test_assert(listMetaLength(objectMetaGetPtr(object_meta),SEGMENT_TYPE_BOTH) == 3 &&
                listMetaLength(objectMetaGetPtr(object_meta),SEGMENT_TYPE_HOT) == 3);

        resetListTestData();
    }

    TEST("list-data: swapin/swapout case-2") {
        /* hot => cold */
        listMeta *swap_meta;
        objectMeta *object_meta;
        robj *value;
        listDataCtx *purectx = puredatactx;

        listMeta *purelm = listMetaCreate();
        listMetaAppendSegment(purelm,SEGMENT_TYPE_HOT,0,3);
        objectMeta *puremeta = createListObjectMeta(0,purelm);
        swapDataSetNewObjectMeta(puredata,puremeta);  
        swap_meta = listMetaCreate();
        listMetaAppendSegment(swap_meta,SEGMENT_TYPE_COLD,0,3);
        purectx->swap_meta = swap_meta;
        listCleanObject(puredata,puredatactx);
        listSwapOut(puredata,puredatactx,NULL);
        object_meta = lookupMeta(db,purekey);
        test_assert(object_meta == NULL);
        value = lookupKey(db,purekey,LOOKUP_NOTOUCH);
        test_assert(value == NULL);

        /* cold => hot */
        colddata->value = NULL;
        listMeta *delta1_meta = listMetaCreate();
        listMetaAppendSegment(delta1_meta,SEGMENT_TYPE_HOT,0,3);
        metaList *delta1 = metaListBuild(delta1_meta,createQuicklistObject());
        metaListPopulateList(delta1);
        listCreateOrMergeObject(colddata,delta1,colddatactx);
        listSwapIn(colddata,delta1,colddatactx);
        value = lookupKey(db,coldkey,LOOKUP_NOTOUCH);
        test_assert(value != NULL && listTypeLength(value) == 3);
        object_meta = lookupMeta(db,coldkey);
        test_assert(object_meta != NULL && 
                listMetaLength(objectMetaGetPtr(object_meta),SEGMENT_TYPE_BOTH) == 3 &&
                listMetaLength(objectMetaGetPtr(object_meta),SEGMENT_TYPE_HOT) == 3);
        test_assert(keyIsHot(object_meta,value));

        resetListTestData();
    }

    TEST("list-data: arg rewrite") {
        listMeta *meta;
        objectMeta *object_meta;
        void *datactx_;
        robj *key = createStringObject("mylist",3);
        robj *list = createQuicklistObject();
        client *c = createClient(NULL);
        selectDb(c,0);

        listTypePush(list,ele1,LIST_TAIL);
        listTypePush(list,ele2,LIST_TAIL);
        listTypePush(list,ele3,LIST_TAIL);

        swapData *data = createSwapData(db,key,list);
        swapDataSetupMeta(data,OBJ_LIST,-1,&datactx_);
        listDataCtx *datactx = datactx_;
        datactx->arg_reqs[0].mstate_idx = -1;
        datactx->arg_reqs[0].mstate_idx = -1;

        meta = listMetaCreate();
        /* 0~2 (COLD) | 3~4 (HOT) | 5 (COLD) | 6 (HOT) */
        listMetaAppendSegment(meta,SEGMENT_TYPE_COLD,0,3);
        listMetaAppendSegment(meta,SEGMENT_TYPE_HOT,3,2);
        listMetaAppendSegment(meta,SEGMENT_TYPE_COLD,5,1);
        listMetaAppendSegment(meta,SEGMENT_TYPE_HOT,6,1);
        object_meta = createListObjectMeta(0,meta);

        dbAdd(db,key,list);
        dbAddMeta(db,key,object_meta);

        /* lindex */
        datactx->arg_reqs[0].arg_idx = 2;
        datactx->arg_reqs[1].arg_idx = -1;

        rewriteResetClientCommandCString(c,3,"LINDEX","mylist","3");
        listBeforeCall(data,c,datactx_);
        test_assert(!strcmp(c->argv[2]->ptr,"0"));
        clientArgRewritesRestore(c);
        test_assert(!strcmp(c->argv[2]->ptr,"3"));

        rewriteResetClientCommandCString(c,3,"LINDEX","mylist","4");
        listBeforeCall(data,c,datactx_);
        test_assert(!strcmp(c->argv[2]->ptr,"1"));
        clientArgRewritesRestore(c);
        test_assert(!strcmp(c->argv[2]->ptr,"4"));

        rewriteResetClientCommandCString(c,3,"LINDEX","mylist","6");
        listBeforeCall(data,c,datactx_);
        test_assert(!strcmp(c->argv[2]->ptr,"2"));
        clientArgRewritesRestore(c);
        test_assert(!strcmp(c->argv[2]->ptr,"6"));

        rewriteResetClientCommandCString(c,3,"LINDEX","mylist","1"); /* fail */
        listBeforeCall(data,c,datactx_);
        test_assert(!strcmp(c->argv[2]->ptr,"0"));
        clientArgRewritesRestore(c);
        test_assert(!strcmp(c->argv[2]->ptr,"1"));

        /* lrange/ltrim */
        datactx->arg_reqs[0].arg_idx = 2;
        datactx->arg_reqs[1].arg_idx = 3;

        rewriteResetClientCommandCString(c,4,"LRANGE","mylist","3","4");
        listBeforeCall(data,c,datactx_);
        test_assert(!strcmp(c->argv[2]->ptr,"0"));
        test_assert(!strcmp(c->argv[3]->ptr,"1"));
        clientArgRewritesRestore(c);
        test_assert(!strcmp(c->argv[2]->ptr,"3"));
        test_assert(!strcmp(c->argv[3]->ptr,"4"));

        dbDeleteMeta(db,key);

        meta = listMetaCreate();
        /* 0~1 (HOT) | 2~3 (COLD) | 4 (HOT)  */
        listMetaAppendSegment(meta,SEGMENT_TYPE_HOT,0,2);
        listMetaAppendSegment(meta,SEGMENT_TYPE_COLD,2,2);
        listMetaAppendSegment(meta,SEGMENT_TYPE_HOT,4,1);

        object_meta = createListObjectMeta(0,meta);
        dbAddMeta(db,key,object_meta);

        /* lindex */
        datactx->arg_reqs[0].arg_idx = 2;
        datactx->arg_reqs[1].arg_idx = -1;

        rewriteResetClientCommandCString(c,3,"LINDEX","mylist","1");
        listBeforeCall(data,c,datactx_);
        test_assert(!strcmp(c->argv[2]->ptr,"1"));
        clientArgRewritesRestore(c);
        test_assert(!strcmp(c->argv[2]->ptr,"1"));

        rewriteResetClientCommandCString(c,3,"LINDEX","mylist","4");
        listBeforeCall(data,c,datactx_);
        test_assert(!strcmp(c->argv[2]->ptr,"2"));
        clientArgRewritesRestore(c);
        test_assert(!strcmp(c->argv[2]->ptr,"4"));

        /* lrange/ltrim */
        datactx->arg_reqs[0].arg_idx = 2;
        datactx->arg_reqs[1].arg_idx = 3;

        rewriteResetClientCommandCString(c,4,"LRANGE","mylist","4","4");
        listBeforeCall(data,c,datactx_);
        test_assert(!strcmp(c->argv[2]->ptr,"2"));
        test_assert(!strcmp(c->argv[2]->ptr,"2"));
        clientArgRewritesRestore(c);
        test_assert(!strcmp(c->argv[2]->ptr,"4"));
        test_assert(!strcmp(c->argv[2]->ptr,"4"));

        dbDelete(db,key);
        swapDataFree(data,datactx);
        decrRefCount(key);
    }

    TEST("list - rdbLoad & rdbSave hot") {
        rio rdb;
        int type;
        uint8_t byte;
        sds key, rdbhot;
        sds ele1raw = listEncodeSubval(ele1),
            ele2raw = listEncodeSubval(ele2),
            ele3raw = listEncodeSubval(ele3);
        uint64_t V = server.swap_key_version = 0; /* reset to zero so that save & load with same version(0) */
        sds ele1key = listEncodeSubkey(db,hotkey->ptr,V,listGetInitialRidx(0)),
            ele2key = listEncodeSubkey(db,hotkey->ptr,V,listGetInitialRidx(1)),
            ele3key = listEncodeSubkey(db,hotkey->ptr,V,listGetInitialRidx(2));

        /* save hot kvpair */
        rioInitWithBuffer(&rdb,sdsempty());
        test_assert(rdbSaveKeyValuePair(&rdb,hotkey,hot,-1) != -1);

        rdbhot = rdb.io.buffer.ptr;
        rioInitWithBuffer(&rdb,rdbhot);

        /* consume rdb header */
        test_assert((type = rdbLoadType(&rdb)) == RDB_OPCODE_FREQ);
        rioRead(&rdb,&byte,1);
        test_assert((type = rdbLoadType(&rdb)) == RDB_TYPE_LIST_QUICKLIST);
        key = rdbGenericLoadStringObject(&rdb,RDB_LOAD_SDS,NULL);
        test_assert(!sdscmp(key,hotkey->ptr));

        /* consume object */
        rdbKeyLoadData _load, *load = &_load;
        rdbKeyLoadDataInit(load,type,db,key,-1,NOW);
        sds metakey, metaval, subkey, subraw;
        int cont, cf, err;

        listMeta *expected_meta = listMetaCreate(); /* list meta rebuilt when load */
        listMetaAppendSegment(expected_meta,SEGMENT_TYPE_COLD,listGetInitialRidx(0),3);
        sds expected_metakey = rocksEncodeMetaKey(db,hotkey->ptr),
            expected_metaextend = encodeListMeta(expected_meta),
            expected_metaval = rocksEncodeMetaVal(OBJ_LIST,-1,V,expected_metaextend);
        listLoadStart(load,&rdb,&cf,&metakey,&metaval,&err);
        test_assert(cf == META_CF && err == 0);
        test_assert(!sdscmp(metakey,expected_metakey) && !sdscmp(metaval,expected_metaval));
        cont = listLoad(load,&rdb,&cf,&subkey,&subraw,&err);
        test_assert(cf == DATA_CF && cont == 1 && err == 0);
        test_assert(!sdscmp(subraw,ele1raw) && !sdscmp(subkey,ele1key));
        sdsfree(subraw), sdsfree(subkey);
        cont = listLoad(load,&rdb,&cf,&subkey,&subraw,&err);
        test_assert(cf == DATA_CF && cont == 1 && err == 0);
        test_assert(!sdscmp(subraw,ele2raw) && !sdscmp(subkey,ele2key));
        sdsfree(subraw), sdsfree(subkey);
        cont = listLoad(load,&rdb,&cf,&subkey,&subraw,&err);
        test_assert(cf == DATA_CF && cont == 0 && err == 0);
        test_assert(!sdscmp(subraw,ele3raw) && !sdscmp(subkey,ele3key));
        sdsfree(subraw), sdsfree(subkey);

        test_assert(load->loaded_fields == 3);
        test_assert(load->object_type == OBJ_LIST);
        rdbKeyLoadDataDeinit(load);

        sdsfree(rdbhot);
        sdsfree(metakey), sdsfree(metaval);
        sdsfree(key);
        listMetaFree(expected_meta);
        sdsfree(expected_metaextend), sdsfree(expected_metaval), sdsfree(expected_metakey);
        sdsfree(ele1raw), sdsfree(ele2raw), sdsfree(ele3raw);
        sdsfree(ele1key), sdsfree(ele2key), sdsfree(ele3key);
        resetListTestData();
    }

    TEST("list - rdbLoad & rdbSave warm") {
        rio rdb;
        int type;
        uint8_t byte;
        sds key, rdbwarm;
        sds ele1rdbraw = rdbEncodeStringObject(ele1), ele1raw = listEncodeSubval(ele1),
            ele2rdbraw = rdbEncodeStringObject(ele2), ele2raw = listEncodeSubval(ele2),
            ele3rdbraw = rdbEncodeStringObject(ele3), ele3raw = listEncodeSubval(ele3);
        uint64_t V = server.swap_key_version = 0; /* reset to zero so that save & load with same version(0) */
        sds ele1key = listEncodeSubkey(db,warmkey->ptr,V,listGetInitialRidx(0)),
            ele2key = listEncodeSubkey(db,warmkey->ptr,V,listGetInitialRidx(1)),
            ele3key = listEncodeSubkey(db,warmkey->ptr,V,listGetInitialRidx(2));
        sds ele1idx = listEncodeRidx(0),
            ele2idx = listEncodeRidx(1),
            ele3idx = listEncodeRidx(2);

        /* save warm kvpair */
        rioInitWithBuffer(&rdb,sdsempty());

        decodedData _decoded, *decoded = &_decoded;
        decoded->cf = META_CF;
        decoded->dbid = db->id;
        decoded->rdbtype = RDB_TYPE_STRING;
        decoded->key = warmkey->ptr;
        decoded->version = V;

        rdbKeySaveData _save, *save = &_save;
        test_assert(rdbKeySaveDataInit(save,db,(decodedResult*)decoded) == 0/*INIT_SAVE_OK*/);
        test_assert(rdbKeySaveStart(save,&rdb) == 0);
        decoded->subkey = ele2idx, decoded->rdbraw = ele2rdbraw;
        test_assert(rdbKeySave(save,&rdb,decoded) == 0 && save->saved == 2);
        decoded->subkey = ele3idx, decoded->rdbraw = ele3rdbraw;
        test_assert(rdbKeySave(save,&rdb,decoded) == 0 && save->saved == 3);
        test_assert(rdbKeySaveEnd(save,&rdb,0) == 0);

        rdbKeySaveDataDeinit(save);
        decoded->rdbraw = NULL, decoded->subkey = NULL;

        rdbwarm = rdb.io.buffer.ptr;
        rioInitWithBuffer(&rdb,rdbwarm);

        /* consume rdb header */
        test_assert((type = rdbLoadType(&rdb)) == RDB_OPCODE_FREQ);
        rioRead(&rdb,&byte,1);
        test_assert((type = rdbLoadType(&rdb)) == RDB_TYPE_LIST);
        key = rdbGenericLoadStringObject(&rdb,RDB_LOAD_SDS,NULL);
        test_assert(!sdscmp(key,warmkey->ptr));

        /* consume object */
        rdbKeyLoadData _load, *load = &_load;
        rdbKeyLoadDataInit(load,type,db,key,-1,NOW);
        sds metakey, metaval, subkey, subraw;
        int cont, cf, err;

        listMeta *expected_meta = listMetaCreate(); /* list meta rebuilt when load */
        listMetaAppendSegment(expected_meta,SEGMENT_TYPE_COLD,listGetInitialRidx(0),3);
        sds expected_metakey = rocksEncodeMetaKey(db,warmkey->ptr),
            expected_metaextend = encodeListMeta(expected_meta),
            expected_metaval = rocksEncodeMetaVal(OBJ_LIST,-1,V,expected_metaextend);

        listLoadStart(load,&rdb,&cf,&metakey,&metaval,&err);
        test_assert(cf == META_CF && err == 0);
        test_assert(!sdscmp(metakey,expected_metakey));
        test_assert(!sdscmp(metaval,expected_metaval));
        cont = listLoad(load,&rdb,&cf,&subkey,&subraw,&err);
        test_assert(cf == DATA_CF && cont == 1 && err == 0);
        test_assert(!sdscmp(subraw,ele1raw) && !sdscmp(subkey,ele1key));
        sdsfree(subkey), sdsfree(subraw);
        cont = listLoad(load,&rdb,&cf,&subkey,&subraw,&err);
        test_assert(cf == DATA_CF && cont == 1 && err == 0);
        test_assert(!sdscmp(subraw,ele2raw) && !sdscmp(subkey,ele2key));
        sdsfree(subkey), sdsfree(subraw);
        cont = listLoad(load,&rdb,&cf,&subkey,&subraw,&err);
        test_assert(cf == DATA_CF && cont == 0 && err == 0);
        test_assert(!sdscmp(subraw,ele3raw) && !sdscmp(subkey,ele3key));
        sdsfree(subkey), sdsfree(subraw);

        test_assert(load->loaded_fields == 3);
        test_assert(load->object_type == OBJ_LIST);

        sdsfree(rdbwarm);
        sdsfree(metakey), sdsfree(metaval);
        sdsfree(key);
        listMetaFree(expected_meta);
        sdsfree(expected_metaextend), sdsfree(expected_metaval), sdsfree(expected_metakey);
        sdsfree(ele1rdbraw), sdsfree(ele2rdbraw), sdsfree(ele3rdbraw);
        sdsfree(ele1raw), sdsfree(ele2raw), sdsfree(ele3raw);
        sdsfree(ele1key), sdsfree(ele2key), sdsfree(ele3key);
        sdsfree(ele1idx), sdsfree(ele2idx), sdsfree(ele3idx);
        resetListTestData();
    }

    TEST("list - rdbLoad & rdbSave cold") {
        rio rdb;
        int type;
        uint8_t byte;
        sds key, rdbcold;
        sds ele1rdbraw = rdbEncodeStringObject(ele1), ele1raw = listEncodeSubval(ele1),
            ele2rdbraw = rdbEncodeStringObject(ele2), ele2raw = listEncodeSubval(ele2),
            ele3rdbraw = rdbEncodeStringObject(ele3), ele3raw = listEncodeSubval(ele3);
        uint64_t V = server.swap_key_version = 0; /* reset to zero so that save & load with same version(0) */
        sds ele1key = listEncodeSubkey(db,coldkey->ptr,V,listGetInitialRidx(0)),
            ele2key = listEncodeSubkey(db,coldkey->ptr,V,listGetInitialRidx(1)),
            ele3key = listEncodeSubkey(db,coldkey->ptr,V,listGetInitialRidx(2));
        sds ele1idx = listEncodeRidx(0),
            ele2idx = listEncodeRidx(1),
            ele3idx = listEncodeRidx(2);

        /* save cold kvpair */
        rioInitWithBuffer(&rdb,sdsempty());

        decodedResult _decoded_meta;
        decodedMeta *decoded_meta = (decodedMeta*)&_decoded_meta;
        decoded_meta->dbid = db->id;
        decoded_meta->key = sdsdup(coldkey->ptr);
        decoded_meta->cf = META_CF;
        decoded_meta->extend = encodeListMeta(coldlm);
        decoded_meta->expire = -1;
        decoded_meta->object_type = OBJ_LIST;
        decoded_meta->version = V;

        rdbKeySaveData _save, *save = &_save;
        test_assert(rdbKeySaveDataInit(save,db,(decodedResult*)decoded_meta) == 0/*INIT_SAVE_OK*/);
        decodedResultDeinit((decodedResult*)decoded_meta);

        decodedData _decoded, *decoded = &_decoded;
        decoded->dbid = db->id;
        decoded->key = coldkey->ptr;
        decoded->cf = DATA_CF;
        decoded->rdbtype = RDB_TYPE_STRING;
        decoded->version = V;

        test_assert(rdbKeySaveStart(save,&rdb) == 0 && save->saved == 0);

        decoded->subkey = ele1idx, decoded->rdbraw = ele1rdbraw;
        test_assert(rdbKeySave(save,&rdb,decoded) == 0 && save->saved == 1);
        decoded->subkey = ele2idx, decoded->rdbraw = ele2rdbraw;
        test_assert(rdbKeySave(save,&rdb,decoded) == 0 && save->saved == 2);
        decoded->subkey = ele3idx, decoded->rdbraw = ele3rdbraw;
        test_assert(rdbKeySave(save,&rdb,decoded) == 0 && save->saved == 3);

        test_assert(rdbKeySaveEnd(save,&rdb,0) == 0);

        rdbKeySaveDataDeinit(save);
        decoded->rdbraw = NULL, decoded->subkey = NULL;

        rdbcold = rdb.io.buffer.ptr;
        rioInitWithBuffer(&rdb,rdbcold);

        /* consume rdb header */
        test_assert((type = rdbLoadType(&rdb)) == RDB_OPCODE_FREQ);
        rioRead(&rdb,&byte,1);
        test_assert((type = rdbLoadType(&rdb)) == RDB_TYPE_LIST);
        key = rdbGenericLoadStringObject(&rdb,RDB_LOAD_SDS,NULL);
        test_assert(!sdscmp(key,coldkey->ptr));

        /* consume object */
        rdbKeyLoadData _load, *load = &_load;
        rdbKeyLoadDataInit(load,type,db,key,-1,NOW);
        sds metakey, metaval, subkey, subraw;
        int cont, cf, err;

        listMeta *expected_meta = listMetaCreate(); /* list meta rebuilt when load */
        listMetaAppendSegment(expected_meta,SEGMENT_TYPE_COLD,listGetInitialRidx(0),3);
        sds expected_metakey = rocksEncodeMetaKey(db,coldkey->ptr),
            expected_metaextend = encodeListMeta(expected_meta),
            expected_metaval = rocksEncodeMetaVal(OBJ_LIST,-1,V,expected_metaextend);

        listLoadStart(load,&rdb,&cf,&metakey,&metaval,&err);
        test_assert(cf == META_CF && err == 0);
        test_assert(!sdscmp(metakey,expected_metakey));
        test_assert(!sdscmp(metaval,expected_metaval));
        cont = listLoad(load,&rdb,&cf,&subkey,&subraw,&err);
        test_assert(cf == DATA_CF && cont == 1 && err == 0);
        test_assert(!sdscmp(subraw,ele1raw) && !sdscmp(subkey,ele1key));
        sdsfree(subkey), sdsfree(subraw);
        cont = listLoad(load,&rdb,&cf,&subkey,&subraw,&err);
        test_assert(cf == DATA_CF && cont == 1 && err == 0);
        test_assert(!sdscmp(subraw,ele2raw) && !sdscmp(subkey,ele2key));
        sdsfree(subkey), sdsfree(subraw);
        cont = listLoad(load,&rdb,&cf,&subkey,&subraw,&err);
        test_assert(cf == DATA_CF && cont == 0 && err == 0);
        test_assert(!sdscmp(subraw,ele3raw) && !sdscmp(subkey,ele3key));
        sdsfree(subkey), sdsfree(subraw);

        test_assert(load->loaded_fields == 3);
        test_assert(load->object_type == OBJ_LIST);

        sdsfree(rdbcold);
        sdsfree(metakey), sdsfree(metaval);
        sdsfree(key);
        listMetaFree(expected_meta);
        sdsfree(expected_metaextend), sdsfree(expected_metaval), sdsfree(expected_metakey);
        sdsfree(ele1rdbraw), sdsfree(ele2rdbraw), sdsfree(ele3rdbraw);
        sdsfree(ele1raw), sdsfree(ele2raw), sdsfree(ele3raw);
        sdsfree(ele1key), sdsfree(ele2key), sdsfree(ele3key);
        sdsfree(ele1idx), sdsfree(ele2idx), sdsfree(ele3idx);
        resetListTestData();
    }

    TEST("list-data: deinit") {
        cleanListTestData();
        decrRefCount(ele1), decrRefCount(ele2), decrRefCount(ele3);
        decrRefCount(purekey), decrRefCount(hotkey), decrRefCount(warmkey), decrRefCount(coldkey);
    }

    return error;
}

int swapListUtilsTest(int argc, char *argv[], int accurate) {
    UNUSED(argc), UNUSED(argv), UNUSED(accurate);
    int error = 0;
    redisDb *db = server.db;
    robj *list = createQuicklistObject();
    robj *key = createStringObject("key",3);
    robj *ele = createStringObject("ele",3), *poped;
    server.swap_mode = SWAP_MODE_DISK;
    dbAdd(db,key,list);

    TEST("list-utils: maintain hot mata") {
        listTypePush(list,ele,LIST_TAIL);
        ctripListTypePush(list,ele,LIST_TAIL,db,key);
        test_assert(lookupListMeta(db,key) == NULL);

        listMeta *meta = listMetaCreate(), *dbmeta;
        listMetaAppendSegment(meta,SEGMENT_TYPE_HOT,0,2);
        listMetaAppendSegment(meta,SEGMENT_TYPE_COLD,2,2);
        objectMeta *object_meta = createListObjectMeta(0,meta);
        dbAddMeta(db,key,object_meta);

        ctripListTypePush(list,ele,LIST_TAIL,db,key);
        test_assert((dbmeta = lookupListMeta(db,key)) != NULL);
        test_assert(listTypeLength(list) == 3);
        test_assert(listMetaLength(dbmeta,SEGMENT_TYPE_BOTH) == 5);
        test_assert(listMetaLength(dbmeta,SEGMENT_TYPE_HOT) == 3);
        test_assert(dbmeta->num == 3);

        poped = ctripListTypePop(list,LIST_HEAD,db,key);
        test_assert(!strcmp(poped->ptr,"ele"));
        test_assert(listTypeLength(list) == 2);
        test_assert(listMetaLength(dbmeta,SEGMENT_TYPE_BOTH) == 4);
        test_assert(listMetaLength(dbmeta,SEGMENT_TYPE_HOT) == 2);
        test_assert(dbmeta->num == 3);
        decrRefCount(poped);

        poped = ctripListTypePop(list,LIST_HEAD,db,key);
        test_assert(!strcmp(poped->ptr,"ele"));
        test_assert(listTypeLength(list) == 1);
        test_assert(listMetaLength(dbmeta,SEGMENT_TYPE_BOTH) == 3);
        test_assert(listMetaLength(dbmeta,SEGMENT_TYPE_HOT) == 1);
        test_assert(dbmeta->num == 2);
        decrRefCount(poped);

        ctripListTypePush(list,ele,LIST_HEAD,db,key);
        quicklistDelRange(list->ptr,0,1);
        quicklistDelRange(list->ptr,-1,1);
        ctripListMetaDelRange(db,key,1,1);
        test_assert(listTypeLength(list) == 0);
        test_assert(listMetaLength(dbmeta,SEGMENT_TYPE_BOTH) == 2);
        test_assert(listMetaLength(dbmeta,SEGMENT_TYPE_HOT) == 0);
        test_assert(dbmeta->num == 1);

    }

    dbDelete(db,key);
    decrRefCount(key);
    decrRefCount(ele);
    return error;
}

#endif


