/*
** mruby/data.h - Data class
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBY_DATA_H
#define MRUBY_DATA_H 1

#if defined(__cplusplus)
extern "C" {
#endif

struct mrb_data_type {
  const char *struct_name;
  void (*dfree)(mrb_state *mrb, void*);
};

struct RData {
  MRB_OBJECT_HEADER;
  struct iv_tbl *iv;
  struct mrb_data_type *type;
  void *data;
};

struct RData *mrb_data_object_alloc(mrb_state *mrb, struct RClass* klass, void *datap, const struct mrb_data_type *type);

#define Data_Wrap_Struct(mrb,klass,type,ptr)\
  mrb_data_object_alloc(mrb,klass,ptr,type)

#define Data_Make_Struct(mrb,klass,strct,type,sval) (\
  sval = mrb_malloc(mrb, sizeof(strct)),\
  { static const strct zero = { 0 }; *sval = zero},\
  Data_Wrap_Struct(mrb,klass,type,sval)\
)

#define RDATA(obj)         ((struct RData *)((obj).value.p))
#define DATA_PTR(d)        (RDATA(d)->data)
#define DATA_TYPE(d)       (RDATA(d)->type)
void *mrb_get_datatype(mrb_state *mrb, mrb_value, const struct mrb_data_type*);
void *mrb_check_datatype(mrb_state *mrb, mrb_value, const struct mrb_data_type*);
#define Data_Get_Struct(mrb,obj,type,sval) do {\
  sval = mrb_check_datatype(mrb, obj, type); \
} while (0)

#if defined(__cplusplus)
}  /* extern "C" { */
#endif

#endif /* MRUBY_DATA_H */
