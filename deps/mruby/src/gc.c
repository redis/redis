/*
** gc.c - garbage collector for mruby
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include "mruby/class.h"
#include "mruby/array.h"
#include "mruby/string.h"
#include "mruby/hash.h"
#include "mruby/range.h"
#include <string.h>
#include "mruby/struct.h"
#include "mruby/proc.h"
#include "mruby/data.h"
#include "mruby/variable.h"

#ifndef SIZE_MAX
#include <limits.h> // for SIZE_MAX
#endif

/*
  = Tri-color Incremental Garbage Collection

  mruby's GC is Tri-color Incremental GC with Mark & Sweep.
  Algorithm details are omitted.
  Instead, the part about the implementation described below.

  == Object's Color

  Each object to be painted in three colors.

    * White - Unmarked.
    * Gray - Marked, But the child objects are unmarked.
    * Black - Marked, the child objects are also marked.

  == Two white part

  The white has a different part of A and B.
  In sweep phase, the sweep target white is either A or B.
  The sweep target white is switched just before sweep phase.
  e.g. A -> B -> A -> B ...

  All objects are painted white when allocated.
  This white is another the sweep target white.
  For example, if the sweep target white is A, it's B.
  So objects when allocated in sweep phase will be next sweep phase target.
  Therefore, these objects will not be released accidentally in sweep phase.

  == Execution Timing

  GC Execution Time and Each step interval are decided by live objects count.
  List of Adjustment API:

    * gc_interval_ratio_set
    * gc_step_ratio_set

  For details, see the comments for each function.

  = Write Barrier

  mruby implementer, C extension library writer must write a write
  barrier when writing a pointer to an object on object's field.
  Two different write barrier:

    * mrb_field_write_barrier
    * mrb_write_barrier

  For details, see the comments for each function.

*/

#ifdef ENABLE_REGEXP
#include "re.h"
#endif

struct free_obj {
  MRB_OBJECT_HEADER;
  struct RBasic *next;
};

typedef struct {
  union {
    struct free_obj free;
    struct RBasic basic;
    struct RObject object;
    struct RClass klass;
    struct RString string;
    struct RArray array;
    struct RHash hash;
    struct RRange range;
#ifdef ENABLE_STRUCT
    struct RStruct structdata;
#endif
    struct RProc procdata;
#ifdef ENABLE_REGEXP
    struct RMatch match;
    struct RRegexp regexp;
#endif
  } as;
} RVALUE;

#ifdef GC_PROFILE
#include <stdio.h>
#include <sys/time.h>

static double program_invoke_time = 0;
static double gc_time = 0;
static double gc_total_time = 0;

static double
gettimeofday_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

#define GC_INVOKE_TIME_REPORT do {\
  fprintf(stderr, "gc_invoke: %19.3f\n", gettimeofday_time() - program_invoke_time);\
} while(0)

#define GC_TIME_START do {\
  gc_time = gettimeofday_time();\
} while(0)

#define GC_TIME_STOP_AND_REPORT do {\
  gc_time = gettimeofday_time() - gc_time;\
  gc_total_time += gc_time;\
  fprintf(stderr, "gc_state: %d\n", mrb->gc_state);\
  fprintf(stderr, "gc_time: %30.20f\n", gc_time);\
  fprintf(stderr, "gc_total_time: %30.20f\n\n", gc_total_time);\
} while(0)
#else
#define GC_INVOKE_TIME_REPORT
#define GC_TIME_START
#define GC_TIME_STOP_AND_REPORT
#endif

#ifdef GC_DEBUG
#include <assert.h>
#define gc_assert(expect) assert(expect)
#else
#define gc_assert(expect) ((void)0)
#endif

#define GC_STEP_SIZE 1024

void*
mrb_realloc(mrb_state *mrb, void *p, size_t len)
{
  void *p2;

  p2 = (mrb->allocf)(mrb, p, len, mrb->ud);

  if (!p2 && len > 0 && mrb->heaps) {
    mrb_garbage_collect(mrb);
    p2 = (mrb->allocf)(mrb, p, len, mrb->ud);
  }
  return p2;
}

void*
mrb_malloc(mrb_state *mrb, size_t len)
{
  return mrb_realloc(mrb, 0, len);
}

void*
mrb_calloc(mrb_state *mrb, size_t nelem, size_t len)
{
  void *p = NULL;
  size_t size;

  if (nelem <= SIZE_MAX / len) {
    size = nelem * len;
    p = mrb_realloc(mrb, 0, size);

    if (p && size > 0)
      memset(p, 0, size);
  }

  return p;
}

void*
mrb_free(mrb_state *mrb, void *p)
{
  return (mrb->allocf)(mrb, p, 0, mrb->ud);
}

#ifndef MRB_HEAP_PAGE_SIZE
#define MRB_HEAP_PAGE_SIZE 1024
#endif

struct heap_page {
  struct RBasic *freelist;
  struct heap_page *prev;
  struct heap_page *next;
  struct heap_page *free_next;
  struct heap_page *free_prev;
  RVALUE objects[MRB_HEAP_PAGE_SIZE];
};

static void
link_heap_page(mrb_state *mrb, struct heap_page *page)
{
  page->next = mrb->heaps;
  if (mrb->heaps)
    mrb->heaps->prev = page;
  mrb->heaps = page;
}

static void
unlink_heap_page(mrb_state *mrb, struct heap_page *page)
{
  if (page->prev)
    page->prev->next = page->next;
  if (page->next)
    page->next->prev = page->prev;
  if (mrb->heaps == page)
    mrb->heaps = page->next;
  page->prev = NULL;
  page->next = NULL;
}

static void
link_free_heap_page(mrb_state *mrb, struct heap_page *page)
{
  page->free_next = mrb->free_heaps;
  if (mrb->free_heaps) {
    mrb->free_heaps->free_prev = page;
  }
  mrb->free_heaps = page;
}

static void
unlink_free_heap_page(mrb_state *mrb, struct heap_page *page)
{
  if (page->free_prev)
    page->free_prev->free_next = page->free_next;
  if (page->free_next)
    page->free_next->free_prev = page->free_prev;
  if (mrb->free_heaps == page)
    mrb->free_heaps = page->free_next;
  page->free_prev = NULL;
  page->free_next = NULL;
}

static void
add_heap(mrb_state *mrb)
{
  struct heap_page *page = (struct heap_page *)mrb_calloc(mrb, 1, sizeof(struct heap_page));
  RVALUE *p, *e;
  struct RBasic *prev = NULL;

  for (p = page->objects, e=p+MRB_HEAP_PAGE_SIZE; p<e; p++) {
    p->as.free.tt = MRB_TT_FREE;
    p->as.free.next = prev;
    prev = &p->as.basic;
  }
  page->freelist = prev;

  link_heap_page(mrb, page);
  link_free_heap_page(mrb, page);
}

#define DEFAULT_GC_INTERVAL_RATIO 200
#define DEFAULT_GC_STEP_RATIO 200

void
mrb_init_heap(mrb_state *mrb)
{
  mrb->heaps = 0;
  mrb->free_heaps = 0;
  add_heap(mrb);
  mrb->gc_interval_ratio = DEFAULT_GC_INTERVAL_RATIO;
  mrb->gc_step_ratio = DEFAULT_GC_STEP_RATIO;

#ifdef GC_PROFILE
  program_invoke_time = gettimeofday_time();
#endif
}

static void obj_free(mrb_state *mrb, struct RBasic *obj);

void
mrb_free_heap(mrb_state *mrb)
{
  struct heap_page *page = mrb->heaps;
  struct heap_page *tmp;
  RVALUE *p, *e;

  while (page) {
    tmp = page;
    page = page->next;
    for (p = tmp->objects, e=p+MRB_HEAP_PAGE_SIZE; p<e; p++) {
      if (p->as.free.tt != MRB_TT_FREE)
	obj_free(mrb, &p->as.basic);
    }
    mrb_free(mrb, tmp);
  }
}

static void
gc_protect(mrb_state *mrb, struct RBasic *p)
{
  if (mrb->arena_idx > MRB_ARENA_SIZE) {
    /* arena overflow error */
    mrb->arena_idx = MRB_ARENA_SIZE - 4; /* force room in arena */
    mrb_raise(mrb, E_RUNTIME_ERROR, "arena overflow error");
  }
  mrb->arena[mrb->arena_idx++] = p;
}

void
mrb_gc_protect(mrb_state *mrb, mrb_value obj)
{
  if (mrb_special_const_p(obj)) return;
  gc_protect(mrb, mrb_basic(obj));
}

struct RBasic*
mrb_obj_alloc(mrb_state *mrb, enum mrb_vtype ttype, struct RClass *cls)
{
  struct RBasic *p;
  static const RVALUE RVALUE_zero = { { { MRB_TT_FALSE } } };

#ifdef MRB_GC_STRESS
  mrb_garbage_collect(mrb);
#endif
  if (mrb->gc_threshold < mrb->live) {
    mrb_incremental_gc(mrb);
  }
  if (mrb->free_heaps == NULL) {
    add_heap(mrb);
  }

  p = mrb->free_heaps->freelist;
  mrb->free_heaps->freelist = ((struct free_obj*)p)->next;
  if (mrb->free_heaps->freelist == NULL) {
    unlink_free_heap_page(mrb, mrb->free_heaps);
  }

  mrb->live++;
  gc_protect(mrb, p);
  *(RVALUE *)p = RVALUE_zero;
  p->tt = ttype;
  p->c = cls;
  paint_partial_white(mrb, p);
  return p;
}

static inline void
add_gray_list(mrb_state *mrb, struct RBasic *obj)
{
#ifdef MRB_GC_STRESS
  if (obj->tt > MRB_TT_MAXDEFINE) {
    abort();
  }
#endif
  paint_gray(obj);
  obj->gcnext = mrb->gray_list;
  mrb->gray_list = obj;
}

static void
gc_mark_children(mrb_state *mrb, struct RBasic *obj)
{
  gc_assert(is_gray(obj));
  paint_black(obj);
  mrb->gray_list = obj->gcnext;
  mrb_gc_mark(mrb, (struct RBasic*)obj->c);
  switch (obj->tt) {
  case MRB_TT_ICLASS:
    mrb_gc_mark(mrb, (struct RBasic*)((struct RClass*)obj)->super);
    break;

  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
  case MRB_TT_SCLASS:
    {
      struct RClass *c = (struct RClass*)obj;

      mrb_gc_mark_mt(mrb, c);
      mrb_gc_mark(mrb, (struct RBasic*)c->super);
    }
    /* fall through */

  case MRB_TT_OBJECT:
  case MRB_TT_DATA:
    mrb_gc_mark_iv(mrb, (struct RObject*)obj);
    break;

  case MRB_TT_PROC:
    {
      struct RProc *p = (struct RProc*)obj;

      mrb_gc_mark(mrb, (struct RBasic*)p->env);
      mrb_gc_mark(mrb, (struct RBasic*)p->target_class);
    }
    break;

  case MRB_TT_ENV:
    {
      struct REnv *e = (struct REnv*)obj;

      if (e->cioff < 0) {
        int i, len;

        len = (int)e->flags;
        for (i=0; i<len; i++) {
          mrb_gc_mark_value(mrb, e->stack[i]);
        }
      }
    }
    break;

  case MRB_TT_ARRAY:
    {
      struct RArray *a = (struct RArray*)obj;
      size_t i, e;

      for (i=0,e=a->len; i<e; i++) {
        mrb_gc_mark_value(mrb, a->ptr[i]);
      }
    }
    break;

  case MRB_TT_HASH:
    mrb_gc_mark_iv(mrb, (struct RObject*)obj);
    mrb_gc_mark_ht(mrb, (struct RHash*)obj);
    break;

  case MRB_TT_STRING:
    break;

  case MRB_TT_RANGE:
    {
      struct RRange *r = (struct RRange*)obj;

      mrb_gc_mark_value(mrb, r->edges->beg);
      mrb_gc_mark_value(mrb, r->edges->end);
    }
    break;

#ifdef ENABLE_REGEXP
  case MRB_TT_MATCH:
    {
      struct RMatch *m = (struct RMatch*)obj;

      mrb_gc_mark(mrb, (struct RBasic*)m->str);
      mrb_gc_mark(mrb, (struct RBasic*)m->regexp);
    }
    break;
  case MRB_TT_REGEX:
    {
      struct RRegexp *r = (struct RRegexp*)obj;

      mrb_gc_mark(mrb, (struct RBasic*)r->src);
    }
    break;
#endif

#ifdef ENABLE_STRUCT
  case MRB_TT_STRUCT:
    {
      struct RStruct *s = (struct RStruct*)obj;
      long i;
      for (i=0; i<s->len; i++){
        mrb_gc_mark_value(mrb, s->ptr[i]);
      }
    }
    break;
#endif

  default:
    break;
  }
}

void
mrb_gc_mark(mrb_state *mrb, struct RBasic *obj)
{
  if (obj == 0) return;
  if (!is_white(obj)) return;
  gc_assert(!is_dead(mrb, obj));
  add_gray_list(mrb, obj);
}

static void
obj_free(mrb_state *mrb, struct RBasic *obj)
{
  DEBUG(printf("obj_free(%p,tt=%d)\n",obj,obj->tt));
  switch (obj->tt) {
    /* immediate - no mark */
  case MRB_TT_TRUE:
  case MRB_TT_FIXNUM:
  case MRB_TT_SYMBOL:
  case MRB_TT_FLOAT:
    /* cannot happen */
    return;

  case MRB_TT_OBJECT:
    mrb_gc_free_iv(mrb, (struct RObject*)obj);
    break;

  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
  case MRB_TT_SCLASS:
    mrb_gc_free_mt(mrb, (struct RClass*)obj);
    mrb_gc_free_iv(mrb, (struct RObject*)obj);
    break;

  case MRB_TT_ENV:
    {
      struct REnv *e = (struct REnv*)obj;

      if (e->cioff < 0) {
        mrb_free(mrb, e->stack);
        e->stack = 0;
      }
    }
    break;

  case MRB_TT_ARRAY:
    if (obj->flags & MRB_ARY_SHARED)
      mrb_ary_decref(mrb, ((struct RArray*)obj)->aux.shared);
    else
      mrb_free(mrb, ((struct RArray*)obj)->ptr);
    break;

  case MRB_TT_HASH:
    mrb_gc_free_iv(mrb, (struct RObject*)obj);
    mrb_gc_free_ht(mrb, (struct RHash*)obj);
    break;

  case MRB_TT_STRING:
    if (obj->flags & MRB_STR_SHARED)
      mrb_str_decref(mrb, ((struct RString*)obj)->aux.shared);
    else
      mrb_free(mrb, ((struct RString*)obj)->ptr);
    break;

  case MRB_TT_RANGE:
    mrb_free(mrb, ((struct RRange*)obj)->edges);
    break;

#ifdef ENABLE_STRUCT
  case MRB_TT_STRUCT:
    mrb_free(mrb, ((struct RStruct*)obj)->ptr);
    break;
#endif

  case MRB_TT_DATA:
    {
      struct RData *d = (struct RData*)obj;
      if (d->type->dfree) {
        d->type->dfree(mrb, d->data);
      }
      mrb_gc_free_iv(mrb, (struct RObject*)obj);
    }
    break;

  default:
    break;
  }
  obj->tt = MRB_TT_FREE;
}

static void
root_scan_phase(mrb_state *mrb)
{
  int i, j, e;
  mrb_callinfo *ci;

  mrb->gray_list = 0;
  mrb->variable_gray_list = 0;

  mrb_gc_mark_gv(mrb);
  /* mark arena */
  for (i=0,e=mrb->arena_idx; i<e; i++) {
    mrb_gc_mark(mrb, mrb->arena[i]);
  }
  /* mark class hierarchy */
  mrb_gc_mark(mrb, (struct RBasic*)mrb->object_class);
  /* mark exception */
  mrb_gc_mark(mrb, (struct RBasic*)mrb->exc);
  /* mark stack */
  e = mrb->stack - mrb->stbase;
  if (mrb->ci) e += mrb->ci->nregs;
  if (mrb->stbase + e > mrb->stend) e = mrb->stend - mrb->stbase;
  for (i=0; i<e; i++) {
    mrb_gc_mark_value(mrb, mrb->stbase[i]);
  }
  /* mark ensure stack */
  e = (mrb->ci) ? mrb->ci->eidx : 0;
  for (i=0; i<e; i++) {
    mrb_gc_mark(mrb, (struct RBasic*)mrb->ensure[i]);
  }
  /* mark closure */
  for (ci = mrb->cibase; ci <= mrb->ci; ci++) {
    if (!ci) continue;
    mrb_gc_mark(mrb, (struct RBasic*)ci->env);
    mrb_gc_mark(mrb, (struct RBasic*)ci->proc);
    mrb_gc_mark(mrb, (struct RBasic*)ci->target_class);
  }
  /* mark irep pool */
  if (mrb->irep) {
    size_t len = mrb->irep_len;
    if (len > mrb->irep_capa) len = mrb->irep_capa;
    for (i=0; i<len; i++) {
      mrb_irep *irep = mrb->irep[i];
      if (!irep) continue;
      for (j=0; j<irep->plen; j++) {
	mrb_gc_mark_value(mrb, irep->pool[j]);
      }
    }
  }
}

static size_t
gc_gray_mark(mrb_state *mrb, struct RBasic *obj)
{
  size_t children = 0;

  gc_mark_children(mrb, obj);

  switch (obj->tt) {
  case MRB_TT_ICLASS:
    children++;
    break;

  case MRB_TT_CLASS:
  case MRB_TT_SCLASS:
  case MRB_TT_MODULE:
    {
      struct RClass *c = (struct RClass*)obj;

      children += mrb_gc_mark_iv_size(mrb, (struct RObject*)obj);
      children += mrb_gc_mark_mt_size(mrb, c);
      children++;
    }
    break;

  case MRB_TT_OBJECT:
  case MRB_TT_DATA:
    children += mrb_gc_mark_iv_size(mrb, (struct RObject*)obj);
    break;

  case MRB_TT_ENV:
    children += (int)obj->flags;
    break;

  case MRB_TT_ARRAY:
    {
      struct RArray *a = (struct RArray*)obj;
      children += a->len;
    }
    break;

  case MRB_TT_HASH:
    children += mrb_gc_mark_iv_size(mrb, (struct RObject*)obj);
    children += mrb_gc_mark_ht_size(mrb, (struct RHash*)obj);
    break;

  case MRB_TT_PROC:
  case MRB_TT_RANGE:
    children+=2;
    break;

#ifdef ENABLE_REGEXP
  case MRB_TT_MATCH:
    children+=2;
    break;
  case MRB_TT_REGEX:
    children+=1;
    break;
#endif

#ifdef ENABLE_STRUCT
  case MRB_TT_STRUCT:
    {
      struct RStruct *s = (struct RStruct*)obj;
      children += s->len;
    }
    break;
#endif

  default:
    break;
  }
  return children;
}

static size_t
incremental_marking_phase(mrb_state *mrb, size_t limit)
{
  size_t tried_marks = 0;

  while (mrb->gray_list && tried_marks < limit) {
    tried_marks += gc_gray_mark(mrb, mrb->gray_list);
  }

  return tried_marks;
}

static void
final_marking_phase(mrb_state *mrb)
{
  while (mrb->gray_list) {
    gc_mark_children(mrb, mrb->gray_list);
  }
  gc_assert(mrb->gray_list == NULL);
  mrb->gray_list = mrb->variable_gray_list;
  mrb->variable_gray_list = 0;
  while (mrb->gray_list) {
    gc_mark_children(mrb, mrb->gray_list);
  }
  gc_assert(mrb->gray_list == NULL);
}

static void
prepare_incremental_sweep(mrb_state *mrb)
{
  mrb->gc_state = GC_STATE_SWEEP;
  mrb->sweeps = mrb->heaps;
  mrb->gc_live_after_mark = mrb->live;
}

static size_t
incremental_sweep_phase(mrb_state *mrb, size_t limit)
{
  struct heap_page *page = mrb->sweeps;
  size_t tried_sweep = 0;

  while (page && (tried_sweep < limit)) {
    RVALUE *p = page->objects;
    RVALUE *e = p + MRB_HEAP_PAGE_SIZE;
    size_t freed = 0;
    int dead_slot = 1;
    int full = (page->freelist == NULL);

    while (p<e) {
      if (is_dead(mrb, &p->as.basic)) {
        if (p->as.basic.tt != MRB_TT_FREE) {
          obj_free(mrb, &p->as.basic);
          p->as.free.next = page->freelist;
          page->freelist = (struct RBasic*)p;
          freed++;
        }
      }
      else {
        paint_partial_white(mrb, &p->as.basic); /* next gc target */
        dead_slot = 0;
      }
      p++;
    }

    /* free dead slot */
    if (dead_slot && freed < MRB_HEAP_PAGE_SIZE) {
      struct heap_page *next = page->next;

      unlink_heap_page(mrb, page);
      unlink_free_heap_page(mrb, page);
      mrb_free(mrb, page);
      page = next;
    }
    else {
      if (full && freed > 0) {
        link_free_heap_page(mrb, page);
      }
      page = page->next;
    }
    tried_sweep += MRB_HEAP_PAGE_SIZE;
    mrb->live -= freed;
    mrb->gc_live_after_mark -= freed;
  }
  mrb->sweeps = page;
  return tried_sweep;
}

static size_t
incremental_gc(mrb_state *mrb, size_t limit)
{
  switch (mrb->gc_state) {
  case GC_STATE_NONE:
    root_scan_phase(mrb);
    mrb->gc_state = GC_STATE_MARK;
    flip_white_part(mrb);
    return 0;
  case GC_STATE_MARK:
    if (mrb->gray_list) {
      return incremental_marking_phase(mrb, limit);
    }
    else {
      final_marking_phase(mrb);
      prepare_incremental_sweep(mrb);
      return 0;
    }
  case GC_STATE_SWEEP: {
     size_t tried_sweep = 0;
     tried_sweep = incremental_sweep_phase(mrb, limit);
     if (tried_sweep == 0)
       mrb->gc_state = GC_STATE_NONE;
     return tried_sweep;
  }
  default:
    /* unknown state */
    gc_assert(0);
    return 0;
  }
}

void
mrb_incremental_gc(mrb_state *mrb)
{
  size_t limit = 0, result = 0;

  if (mrb->gc_disabled) return;
  GC_INVOKE_TIME_REPORT;
  GC_TIME_START;

  limit = (GC_STEP_SIZE/100) * mrb->gc_step_ratio;
  while (result < limit) {
    result += incremental_gc(mrb, limit);
    if (mrb->gc_state == GC_STATE_NONE)
      break;
  }

  if (mrb->gc_state == GC_STATE_NONE) {
    gc_assert(mrb->live >= mrb->gc_live_after_mark);
    mrb->gc_threshold = (mrb->gc_live_after_mark/100) * mrb->gc_interval_ratio;
    if (mrb->gc_threshold < GC_STEP_SIZE) {
      mrb->gc_threshold = GC_STEP_SIZE;
    }
  }
  else {
    mrb->gc_threshold = mrb->live + GC_STEP_SIZE;
  }


  GC_TIME_STOP_AND_REPORT;
}

void
mrb_garbage_collect(mrb_state *mrb)
{
  size_t max_limit = ~0;

  if (mrb->gc_disabled) return;
  GC_INVOKE_TIME_REPORT;
  GC_TIME_START;

  if (mrb->gc_state == GC_STATE_SWEEP) {
    /* finish sweep phase */
    while (mrb->gc_state != GC_STATE_NONE) {
      incremental_gc(mrb, max_limit);
    }
  }

  do {
    incremental_gc(mrb, max_limit);
  } while (mrb->gc_state != GC_STATE_NONE);

  mrb->gc_threshold = (mrb->gc_live_after_mark/100) * mrb->gc_interval_ratio;

  GC_TIME_STOP_AND_REPORT;
}

int
mrb_gc_arena_save(mrb_state *mrb)
{
  return mrb->arena_idx;
}

void
mrb_gc_arena_restore(mrb_state *mrb, int idx)
{
  mrb->arena_idx = idx;
}

/*
 * Field write barrier
 *   Paint obj(Black) -> value(White) to obj(Black) -> value(Black).
 */

void
mrb_field_write_barrier(mrb_state *mrb, struct RBasic *obj, struct RBasic *value)
{
  if (!is_black(obj)) return;
  if (!is_white(value)) return;

  gc_assert(!is_dead(mrb, value) && !is_dead(mrb, obj));
  gc_assert(mrb->gc_state != GC_STATE_NONE);

  if (mrb->gc_state == GC_STATE_MARK) {
    add_gray_list(mrb, value);
  }
  else {
    gc_assert(mrb->gc_state == GC_STATE_SWEEP);
    paint_partial_white(mrb, obj); /* for never write barriers */
  }
}

/*
 * Write barrier
 *   Paint obj(Black) to obj(Gray).
 *
 *   The object that is painted gray will be traversed atomically in final
 *   mark phase. So you use this write barrier if it's frequency written spot.
 *   e.g. Set element on Array.
 */

void
mrb_write_barrier(mrb_state *mrb, struct RBasic *obj)
{
  if (!is_black(obj)) return;

  gc_assert(!is_dead(mrb, obj));
  gc_assert(mrb->gc_state != GC_STATE_NONE);
  paint_gray(obj);
  obj->gcnext = mrb->variable_gray_list;
  mrb->variable_gray_list = obj;
}

/*
 *  call-seq:
 *     GC.start                     -> nil
 *
 *  Initiates full garbage collection.
 *
 */

static mrb_value
gc_start(mrb_state *mrb, mrb_value obj)
{
  mrb_garbage_collect(mrb);
  return mrb_nil_value();
}

/*
 *  call-seq:
 *     GC.enable    -> true or false
 *
 *  Enables garbage collection, returning <code>true</code> if garbage
 *  collection was previously disabled.
 *
 *     GC.disable   #=> false
 *     GC.enable    #=> true
 *     GC.enable    #=> false
 *
 */

static mrb_value
gc_enable(mrb_state *mrb, mrb_value obj)
{
  int old = mrb->gc_disabled;

  mrb->gc_disabled = FALSE;
  if (old) return mrb_true_value();
  return mrb_false_value();
}

/*
 *  call-seq:
 *     GC.disable    -> true or false
 *
 *  Disables garbage collection, returning <code>true</code> if garbage
 *  collection was already disabled.
 *
 *     GC.disable   #=> false
 *     GC.disable   #=> true
 *
 */

static mrb_value
gc_disable(mrb_state *mrb, mrb_value obj)
{
  int old = mrb->gc_disabled;

  mrb->gc_disabled = TRUE;
  if (old) return mrb_true_value();
  return mrb_false_value();
}

/*
 *  call-seq:
 *     GC.interval_ratio      -> fixnum
 *
 *  Returns ratio of GC interval. Default value is 200(%).
 *
 */

static mrb_value
gc_interval_ratio_get(mrb_state *mrb, mrb_value obj)
{
  return mrb_fixnum_value(mrb->gc_interval_ratio);
}

/*
 *  call-seq:
 *     GC.interval_ratio = fixnum    -> nil
 *
 *  Updates ratio of GC interval. Default value is 200(%).
 *  GC start as soon as after end all step of GC if you set 100(%).
 *
 */

static mrb_value
gc_interval_ratio_set(mrb_state *mrb, mrb_value obj)
{
  mrb_int ratio;

  mrb_get_args(mrb, "i", &ratio);
  mrb->gc_interval_ratio = ratio;
  return mrb_nil_value();
}

/*
 *  call-seq:
 *     GC.step_ratio    -> fixnum
 *
 *  Returns step span ratio of Incremental GC. Default value is 200(%).
 *
 */

static mrb_value
gc_step_ratio_get(mrb_state *mrb, mrb_value obj)
{
  return mrb_fixnum_value(mrb->gc_step_ratio);
}

/*
 *  call-seq:
 *     GC.step_ratio = fixnum   -> nil
 *
 *  Updates step span ratio of Incremental GC. Default value is 200(%).
 *  1 step of incrementalGC becomes long if a rate is big.
 *
 */

static mrb_value
gc_step_ratio_set(mrb_state *mrb, mrb_value obj)
{
  mrb_int ratio;

  mrb_get_args(mrb, "i", &ratio);
  mrb->gc_step_ratio = ratio;
  return mrb_nil_value();
}

void
mrb_init_gc(mrb_state *mrb)
{
  struct RClass *gc;
  gc = mrb_define_module(mrb, "GC");

  mrb_define_class_method(mrb, gc, "start", gc_start, ARGS_NONE());
  mrb_define_class_method(mrb, gc, "enable", gc_enable, ARGS_NONE());
  mrb_define_class_method(mrb, gc, "disable", gc_disable, ARGS_NONE());
  mrb_define_class_method(mrb, gc, "interval_ratio", gc_interval_ratio_get, ARGS_NONE());
  mrb_define_class_method(mrb, gc, "interval_ratio=", gc_interval_ratio_set, ARGS_REQ(1));
  mrb_define_class_method(mrb, gc, "step_ratio", gc_step_ratio_get, ARGS_NONE());
  mrb_define_class_method(mrb, gc, "step_ratio=", gc_step_ratio_set, ARGS_REQ(1));
}

#ifdef GC_TEST
#ifdef GC_DEBUG
void
test_mrb_field_write_barrier(void)
{
  mrb_state *mrb = mrb_open();
  struct RBasic *obj, *value;

  puts("test_mrb_field_write_barrier");
  obj = mrb_basic(mrb_ary_new(mrb));
  value = mrb_basic(mrb_str_new_cstr(mrb, "value"));
  paint_black(obj);
  paint_partial_white(mrb,value);


  puts("  in GC_STATE_MARK");
  mrb->gc_state = GC_STATE_MARK;
  mrb_field_write_barrier(mrb, obj, value);

  gc_assert(is_gray(value));


  puts("  in GC_STATE_SWEEP");
  paint_partial_white(mrb,value);
  mrb->gc_state = GC_STATE_SWEEP;
  mrb_field_write_barrier(mrb, obj, value);

  gc_assert(obj->color & mrb->current_white_part);
  gc_assert(obj->color & mrb->current_white_part);


  puts("  fail with black");
  mrb->gc_state = GC_STATE_MARK;
  paint_white(obj);
  paint_partial_white(mrb,value);
  mrb_field_write_barrier(mrb, obj, value);

  gc_assert(obj->color & mrb->current_white_part);


  puts("  fail with gray");
  mrb->gc_state = GC_STATE_MARK;
  paint_black(obj);
  paint_gray(value);
  mrb_field_write_barrier(mrb, obj, value);

  gc_assert(is_gray(value));


  {
    puts("test_mrb_field_write_barrier_value");
    obj = mrb_basic(mrb_ary_new(mrb));
    mrb_value value = mrb_str_new_cstr(mrb, "value");
    paint_black(obj);
    paint_partial_white(mrb, mrb_basic(value));

    mrb->gc_state = GC_STATE_MARK;
    mrb_field_write_barrier_value(mrb, obj, value);

    gc_assert(is_gray(mrb_basic(value)));
  }

  mrb_close(mrb);
}

void
test_mrb_write_barrier(void)
{
  mrb_state *mrb = mrb_open();
  struct RBasic *obj;

  puts("test_mrb_write_barrier");
  obj = mrb_basic(mrb_ary_new(mrb));
  paint_black(obj);

  puts("  in GC_STATE_MARK");
  mrb->gc_state = GC_STATE_MARK;
  mrb_write_barrier(mrb, obj);

  gc_assert(is_gray(obj));
  gc_assert(mrb->variable_gray_list == obj);


  puts("  fail with gray");
  paint_gray(obj);
  mrb_write_barrier(mrb, obj);

  gc_assert(is_gray(obj));

  mrb_close(mrb);
}

void
test_add_gray_list(void)
{
  mrb_state *mrb = mrb_open();
  struct RBasic *obj1, *obj2;

  puts("test_add_gray_list");
  gc_assert(mrb->gray_list == NULL);
  obj1 = mrb_basic(mrb_str_new_cstr(mrb, "test"));
  add_gray_list(mrb, obj1);
  gc_assert(mrb->gray_list == obj1);
  gc_assert(is_gray(obj1));

  obj2 = mrb_basic(mrb_str_new_cstr(mrb, "test"));
  add_gray_list(mrb, obj2);
  gc_assert(mrb->gray_list == obj2);
  gc_assert(mrb->gray_list->gcnext == obj1);
  gc_assert(is_gray(obj2));

  mrb_close(mrb);
}

void
test_gc_gray_mark(void)
{
  mrb_state *mrb = mrb_open();
  mrb_value obj_v, value_v;
  struct RBasic *obj;
  size_t gray_num = 0;

  puts("test_gc_gray_mark");

  puts("  in MRB_TT_CLASS");
  obj = (struct RBasic*)mrb->object_class;
  paint_gray(obj);
  gray_num = gc_gray_mark(mrb, obj);
  gc_assert(is_black(obj));
  gc_assert(gray_num > 1);

  puts("  in MRB_TT_ARRAY");
  obj_v = mrb_ary_new(mrb);
  value_v = mrb_str_new_cstr(mrb, "test");
  paint_gray(mrb_basic(obj_v));
  paint_partial_white(mrb, mrb_basic(value_v));
  mrb_ary_push(mrb, obj_v, value_v);
  gray_num = gc_gray_mark(mrb, mrb_basic(obj_v));
  gc_assert(is_black(mrb_basic(obj_v)));
  gc_assert(is_gray(mrb_basic(value_v)));
  gc_assert(gray_num == 1);

  mrb_close(mrb);
}

void
test_incremental_gc(void)
{
  mrb_state *mrb = mrb_open();
  size_t max = ~0, live = 0, total = 0, freed = 0;
  RVALUE *free;
  struct heap_page *page;

  puts("test_incremental_gc");

  mrb_garbage_collect(mrb);

  gc_assert(mrb->gc_state == GC_STATE_NONE);
  incremental_gc(mrb, max);
  gc_assert(mrb->gc_state == GC_STATE_MARK);

  incremental_gc(mrb, max);
  gc_assert(mrb->gc_state == GC_STATE_MARK);

  incremental_gc(mrb, max);
  gc_assert(mrb->gc_state == GC_STATE_SWEEP);

  page = mrb->heaps;
  while (page) {
    RVALUE *p = page->objects;
    RVALUE *e = p + MRB_HEAP_PAGE_SIZE;
    while (p<e) {
      if (is_black(&p->as.basic)) {
        live++;
      }
      if (is_gray(&p->as.basic) && !is_dead(mrb, &p->as.basic)) {
        printf("%p\n", &p->as.basic);
      }
      p++;
    }
    page = page->next;
    total += MRB_HEAP_PAGE_SIZE;
  }

  gc_assert(mrb->gray_list == NULL);

  incremental_gc(mrb, max);
  gc_assert(mrb->gc_state == GC_STATE_SWEEP);

  incremental_gc(mrb, max);
  gc_assert(mrb->gc_state == GC_STATE_NONE);

  free = (RVALUE*)mrb->heaps->freelist;
  while (free) {
   freed++;
   free = (RVALUE*)free->as.free.next;
  }

  gc_assert(mrb->live == live);
  gc_assert(mrb->live == total-freed);

  mrb_close(mrb);
}

void
test_incremental_sweep_phase(void)
{
  mrb_state *mrb = mrb_open();

  puts("test_incremental_sweep_phase");

  add_heap(mrb);
  mrb->sweeps = mrb->heaps;

  gc_assert(mrb->heaps->next->next == NULL);
  gc_assert(mrb->free_heaps->next->next == NULL);
  incremental_sweep_phase(mrb, MRB_HEAP_PAGE_SIZE*3);

  gc_assert(mrb->heaps->next == NULL);
  gc_assert(mrb->heaps == mrb->free_heaps);

  mrb_close(mrb);
}

void
test_gc_api(void)
{
  mrb_state *mrb = mrb_open();
  mrb_value res;

  mrb_value argv[1];

  puts("test_gc_api");

  gc_start(mrb, mrb_nil_value());

  res = gc_interval_ratio_get(mrb, mrb_nil_value());
  gc_assert(mrb_fixnum(res) == 200);

  argv[0] = mrb_fixnum_value(300);
  mrb->argv = &argv;
  mrb->argc = 1;

  gc_interval_ratio_set(mrb, mrb_nil_value());
  res = gc_interval_ratio_get(mrb, mrb_nil_value());
  gc_assert(mrb_fixnum(res) == 300);

  res = gc_step_ratio_get(mrb, mrb_nil_value());
  gc_assert(mrb_fixnum(res) == 200);

  gc_step_ratio_set(mrb, mrb_nil_value());
  res = gc_step_ratio_get(mrb, mrb_nil_value());
  gc_assert(mrb_fixnum(res) == 300);

  mrb_close(mrb);
}

static void
test_many_object_benchmark(void)
{
  mrb_state *mrb = mrb_open();
  size_t i = 0, j=0;
  mrb_value ary = mrb_ary_new(mrb);
  int save_point = mrb_gc_arena_save(mrb);

  puts("test_many_object_benchmark");

  for (i=0; i<1000; i++) {
    mrb_value cary = mrb_ary_new(mrb);
    mrb_ary_push(mrb, ary, cary);
    for (j=0; j<1000; j++) {
      mrb_ary_push(mrb, cary, mrb_str_new_cstr(mrb, "t"));
    }
    mrb_gc_arena_restore(mrb, save_point);
  }

  mrb_close(mrb);
}

int
main(void)
{
  test_mrb_field_write_barrier();
  test_mrb_write_barrier();
  test_add_gray_list();
  test_gc_gray_mark();
  test_incremental_gc();
  test_incremental_sweep_phase();
  test_gc_api();
  test_many_object_benchmark();
  return 0;
}
#endif
#endif
