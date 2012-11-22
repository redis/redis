/*
** mruby/struct.h - Struct class
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBY_STRUCT_H
#define MRUBY_STRUCT_H

#if defined(__cplusplus)
extern "C" {
#endif

struct RStruct {
    struct RBasic basic;
    long len;
    mrb_value *ptr;
};
#define RSTRUCT(st)     ((struct RStruct*)((st).value.p))
#define RSTRUCT_LEN(st) ((int)(RSTRUCT(st)->len))
#define RSTRUCT_PTR(st) (RSTRUCT(st)->ptr)

#if defined(__cplusplus)
}  /* extern "C" { */
#endif

#endif /* MRUBY_STRUCT_H */
