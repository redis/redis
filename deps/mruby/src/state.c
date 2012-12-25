/*
** state.c - mrb_state open/close functions
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include "mruby/irep.h"
#include "mruby/variable.h"
#include <string.h>

void mrb_init_heap(mrb_state*);
void mrb_init_core(mrb_state*);
void mrb_init_ext(mrb_state*);

mrb_state*
mrb_open_allocf(mrb_allocf f, void *ud)
{
  static const mrb_state mrb_state_zero = { 0 };
  mrb_state *mrb = (mrb_state *)(f)(NULL, NULL, sizeof(mrb_state), ud);
  if (mrb == NULL) return NULL;

  *mrb = mrb_state_zero;
  mrb->ud = ud;
  mrb->allocf = f;
  mrb->current_white_part = MRB_GC_WHITE_A;

  mrb_init_heap(mrb);
  mrb_init_core(mrb);
  mrb_init_ext(mrb);
  return mrb;
}

static void*
allocf(mrb_state *mrb, void *p, size_t size, void *ud)
{
  if (size == 0) {
    free(p);
    return NULL;
  }
  else {
    return realloc(p, size);
  }
}

struct alloca_header {
  struct alloca_header *next;
  char buf[0];
};

void*
mrb_alloca(mrb_state *mrb, size_t size)
{
  struct alloca_header *p;

  p = (struct alloca_header*) mrb_malloc(mrb, sizeof(struct alloca_header)+size);
  p->next = mrb->mems;
  mrb->mems = p;
  return (void*)p->buf;
}

static void
mrb_alloca_free(mrb_state *mrb)
{
  struct alloca_header *p = mrb->mems;
  struct alloca_header *tmp;

  while (p) {
    tmp = p;
    p = p->next;
    mrb_free(mrb, tmp);
  }
}

mrb_state*
mrb_open()
{
  mrb_state *mrb = mrb_open_allocf(allocf, NULL);

  return mrb;
}

void mrb_free_symtbl(mrb_state *mrb);
void mrb_free_heap(mrb_state *mrb);

void
mrb_close(mrb_state *mrb)
{
  int i;

  /* free */
  mrb_gc_free_gv(mrb);
  mrb_free(mrb, mrb->stbase);
  mrb_free(mrb, mrb->cibase);
  for (i=0; i<mrb->irep_len; i++) {
    if (!(mrb->irep[i]->flags & MRB_ISEQ_NO_FREE))
      mrb_free(mrb, mrb->irep[i]->iseq);
    mrb_free(mrb, mrb->irep[i]->pool);
    mrb_free(mrb, mrb->irep[i]->syms);
    mrb_free(mrb, mrb->irep[i]->lines);
    mrb_free(mrb, mrb->irep[i]);
  }
  mrb_free(mrb, mrb->irep);
  mrb_free(mrb, mrb->rescue);
  mrb_free(mrb, mrb->ensure);
  mrb_free_symtbl(mrb);
  mrb_free_heap(mrb);
  mrb_alloca_free(mrb);
  mrb_free(mrb, mrb);
}

mrb_irep*
mrb_add_irep(mrb_state *mrb)
{
  static const mrb_irep mrb_irep_zero = { 0 };
  mrb_irep *irep;

  if (!mrb->irep) {
    int max = 256;

    if (mrb->irep_len > max) max = mrb->irep_len+1;
    mrb->irep = (mrb_irep **)mrb_calloc(mrb, max, sizeof(mrb_irep*));
    mrb->irep_capa = max;
  }
  else if (mrb->irep_capa <= mrb->irep_len) {
    int i;
    size_t old_capa = mrb->irep_capa;
    while (mrb->irep_capa <= mrb->irep_len) {
      mrb->irep_capa *= 2;
    }
    mrb->irep = (mrb_irep **)mrb_realloc(mrb, mrb->irep, sizeof(mrb_irep*)*mrb->irep_capa);
    for (i = old_capa; i < mrb->irep_capa; i++) {
      mrb->irep[i] = NULL;
    }
  }
  irep = (mrb_irep *)mrb_malloc(mrb, sizeof(mrb_irep));
  *irep = mrb_irep_zero;
  mrb->irep[mrb->irep_len] = irep;
  irep->idx = mrb->irep_len++;

  return irep;
}

mrb_value
mrb_top_self(mrb_state *mrb)
{
  mrb_value v;

  MRB_SET_VALUE(v, MRB_TT_MAIN, value.i, 0);
  return v;
}
