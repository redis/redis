/*
** cdump.c - mruby binary dumper (C source format)
**
** See Copyright Notice in mruby.h
*/

#include "mruby/cdump.h"

#include <string.h>

#include "mruby/irep.h"
#include "mruby/string.h"

#define MRB_CDUMP_LINE_LEN 128

#define SOURCE_CODE(fmt, ...)  fprintf(f, fmt"\n", __VA_ARGS__)
#define SOURCE_CODE0(str)      do {fputs(str, f); putc('\n', f);} while (0)

static int
make_cdump_isec(mrb_state *mrb, int irep_no, FILE *f)
{
  int i;
  mrb_irep *irep = mrb->irep[irep_no];

  if (irep == NULL)
    return -1;

  /* dump isec struct*/
  if (irep->ilen > 0) {
    SOURCE_CODE  ("static mrb_code iseq_%d[] = {", irep_no);
    for (i=0; i<irep->ilen; i++)
      SOURCE_CODE("  0x%08x,"                    , irep->iseq[i]);
    SOURCE_CODE0 ("};");
    SOURCE_CODE0 ("");
  }

  return 0;
}

static size_t
str_format_len(mrb_value str)
{
  size_t dump_len = 0;

  char *src;

  for (src = RSTRING_PTR(str); src < RSTRING_END(str); src++) {
    switch (*src) {
    case 0x07:/* BEL */ /* fall through */
    case 0x08:/* BS  */ /* fall through */
    case 0x09:/* HT  */ /* fall through */
    case 0x0A:/* LF  */ /* fall through */
    case 0x0B:/* VT  */ /* fall through */
    case 0x0C:/* FF  */ /* fall through */
    case 0x0D:/* CR  */ /* fall through */
    case 0x22:/* "   */ /* fall through */
    case 0x27:/* '   */ /* fall through */
    case 0x3F:/* ?   */ /* fall through */
    case 0x5C:/* \   */ /* fall through */
      dump_len += 2;
      break;

    default:
      dump_len++;
      break;
    }
  }

  return dump_len;
}

static char*
str_to_format(mrb_value str, char *buf)
{
  char *src;
  char *dst;

  for (src = RSTRING_PTR(str), dst = buf; src < RSTRING_END(str); src++) {
    switch (*src) {
    case 0x07:/* BEL */  *dst++ = '\\'; *dst++ = 'a'; break;
    case 0x08:/* BS  */  *dst++ = '\\'; *dst++ = 'b'; break;
    case 0x09:/* HT  */  *dst++ = '\\'; *dst++ = 't'; break;
    case 0x0A:/* LF  */  *dst++ = '\\'; *dst++ = 'n'; break;
    case 0x0B:/* VT  */  *dst++ = '\\'; *dst++ = 'v'; break;
    case 0x0C:/* FF  */  *dst++ = '\\'; *dst++ = 'f'; break;
    case 0x0D:/* CR  */  *dst++ = '\\'; *dst++ = 'r'; break;
    case 0x22:/* "   */  *dst++ = '\\'; *dst++ = '\"'; break;
    case 0x27:/* '   */  *dst++ = '\\'; *dst++ = '\''; break;
    case 0x3F:/* ?   */  *dst++ = '\\'; *dst++ = '\?'; break;
    case 0x5C:/* \   */  *dst++ = '\\'; *dst++ = '\\'; break;
    default: *dst++ = *src; break;
    }
  }

  return buf;
}

int
make_cdump_irep(mrb_state *mrb, int irep_no, FILE *f)
{
  mrb_irep *irep = mrb->irep[irep_no];
  int n;
  char *buf = 0;
  size_t buf_len, str_len;

  if (irep == NULL)
    return -1;

  buf_len = MRB_CDUMP_LINE_LEN;
  if ((buf = (char *)mrb_malloc(mrb, buf_len)) == NULL) {
    return MRB_CDUMP_GENERAL_FAILURE;
  }

  SOURCE_CODE0     ("  ai = mrb->arena_idx;");
  SOURCE_CODE0     ("  irep = mrb_add_irep(mrb);");
  SOURCE_CODE0     ("  irep->flags = MRB_ISEQ_NO_FREE;");
  SOURCE_CODE      ("  irep->nlocals = %d;",                                          irep->nlocals);
  SOURCE_CODE      ("  irep->nregs = %d;",                                            irep->nregs);
  SOURCE_CODE      ("  irep->ilen = %d;",                                             irep->ilen);
  SOURCE_CODE      ("  irep->iseq = iseq_%d;",                                        irep_no);

  SOURCE_CODE      ("  irep->slen = %d;",                                             irep->slen);
  if(irep->slen > 0) {
    SOURCE_CODE    ("  irep->syms = mrb_malloc(mrb, sizeof(mrb_sym)*%d);",            irep->slen);
    for (n=0; n<irep->slen; n++)
      if (irep->syms[n]) {
	const char *name;
	int len;

	name = mrb_sym2name_len(mrb, irep->syms[n], &len);
        SOURCE_CODE  ("  irep->syms[%d] = mrb_intern2(mrb, \"%s\", %d);",             n, name, len);
      }
  }
  else
    SOURCE_CODE0   ("  irep->syms = NULL;");

  SOURCE_CODE0     ("  irep->pool = NULL;");
  SOURCE_CODE0     ("  irep->lines = NULL;");
  SOURCE_CODE0     ("  mrb->irep_len = idx;");
  SOURCE_CODE0     ("  irep->plen = 0;");
  if(irep->plen > 0) {
    SOURCE_CODE    ("  irep->pool = mrb_malloc(mrb, sizeof(mrb_value)*%d);",          irep->plen);
    for (n=0; n<irep->plen; n++) {
      switch (mrb_type(irep->pool[n])) {
      case MRB_TT_FLOAT:
        SOURCE_CODE("  irep->pool[%d] = mrb_float_value(%.16e);",                     n, mrb_float(irep->pool[n])); break;
      case MRB_TT_FIXNUM:
        SOURCE_CODE("  irep->pool[%d] = mrb_fixnum_value(%d);",                       n, mrb_fixnum(irep->pool[n])); break; 
     case MRB_TT_STRING:
        str_len = str_format_len(irep->pool[n]) + 1;
        if ( str_len > buf_len ) {
          buf_len = str_len;
          if ((buf = (char *)mrb_realloc(mrb, buf, buf_len)) == NULL) {
            return MRB_CDUMP_GENERAL_FAILURE;
          }
        }
        memset(buf, 0, buf_len);
        SOURCE_CODE("  irep->pool[%d] = mrb_str_new(mrb, \"%s\", %d);",               n, str_to_format(irep->pool[n], buf), RSTRING_LEN(irep->pool[n]));
	SOURCE_CODE0     ("  mrb->arena_idx = ai;");
	break;
      /* TODO MRB_TT_REGEX */
      default: break;
      }
      SOURCE_CODE0("  irep->plen++;");
    }
  }
  else
  SOURCE_CODE0("");

  if (buf)
    mrb_free(mrb, buf);

  return MRB_CDUMP_OK;
}

int
mrb_cdump_irep(mrb_state *mrb, int n, FILE *f,const char *initname)
{
  int irep_no;

  if (mrb == NULL || n < 0 || n >= mrb->irep_len || f == NULL || initname == NULL)
    return -1;

  SOURCE_CODE0("#include \"mruby.h\"");
  SOURCE_CODE0("#include \"mruby/irep.h\"");
  SOURCE_CODE0("#include \"mruby/string.h\"");
  SOURCE_CODE0("#include \"mruby/proc.h\"");
  SOURCE_CODE0("");

  for (irep_no=n; irep_no<mrb->irep_len; irep_no++) {
    if (make_cdump_isec(mrb, irep_no, f) != 0)
      return -1;
  }

  SOURCE_CODE0("void");
  SOURCE_CODE ("%s(mrb_state *mrb)",            initname);
  SOURCE_CODE0("{");
  SOURCE_CODE0("  int n = mrb->irep_len;");
  SOURCE_CODE0("  int idx = n;");
  SOURCE_CODE0("  int ai;");
  SOURCE_CODE0("  mrb_irep *irep;");
  SOURCE_CODE0("");
  for (irep_no=n; irep_no<mrb->irep_len; irep_no++) {
    if (make_cdump_irep(mrb, irep_no, f) != 0)
      return -1;
  }

  SOURCE_CODE0("  mrb_run(mrb, mrb_proc_new(mrb, mrb->irep[n]), mrb_top_self(mrb));");
  SOURCE_CODE0("}");

  return 0;
}
