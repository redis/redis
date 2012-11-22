/*
** opcode.h - RiteVM operation codes
**
** See Copyright Notice in mruby.h
*/

#ifndef OPCODE_H
#define OPCODE_H

#define MAXARG_Bx        ((1<<16)-1)
#define MAXARG_sBx       (MAXARG_Bx>>1)         /* `sBx' is signed */

/* instructions OP:A:B:C = 7:9:9:7 (32 bits) */
/*              OP:A:Bx  = 7:9:16            */
/*              OP:Ax    = 7:25              */

#define GET_OPCODE(i) ((int)(((mrb_code)(i)) & 0x7f))
#define GETARG_A(i)   ((int)((((mrb_code)(i)) >> 23) & 0x1ff))
#define GETARG_B(i)   ((int)((((mrb_code)(i)) >> 14) & 0x1ff))
#define GETARG_C(i)   ((int)((((mrb_code)(i)) >>  7) & 0x7f))
#define GETARG_Bx(i)  ((int)((((mrb_code)(i)) >>  7) & 0xffff))
#define GETARG_sBx(i) ((int)(GETARG_Bx(i)-MAXARG_sBx))
#define GETARG_Ax(i)  ((int)((((mrb_code)(i)) >>  7) & 0x1ffffff))
#define GETARG_UNPACK_b(i,n1,n2) ((int)((((mrb_code)(i)) >> (7+n2)) & (((1<<n1)-1))))
#define GETARG_UNPACK_c(i,n1,n2) ((int)((((mrb_code)(i)) >> 7) & (((1<<n2)-1))))
#define GETARG_b(i)   GETARG_UNPACK_b(i,14,2)
#define GETARG_c(i)   GETARG_UNPACK_c(i,14,2)

#define MKOPCODE(op)  ((op) & 0x7f)
#define MKARG_A(c)    (((c) & 0x1ff) << 23)
#define MKARG_B(c)    (((c) & 0x1ff) << 14)
#define MKARG_C(c)    (((c) & 0x7f) <<  7)
#define MKARG_Bx(v)   (((v) & 0xffff) << 7)
#define MKARG_sBx(v)  MKARG_Bx((v)+MAXARG_sBx)
#define MKARG_Ax(v)   (((v) & 0x1ffffff) << 7)
#define MKARG_PACK(b,n1,c,n2) ((((b) & ((1<<n1)-1)) << (7+n2))|(((c) & ((1<<n2)-1)) << 7))
#define MKARG_bc(b,c) MKARG_PACK(b,14,c,2)

#define MKOP_A(op,a)        (MKOPCODE(op)|MKARG_A(a))
#define MKOP_AB(op,a,b)     (MKOP_A(op,a)|MKARG_B(b))
#define MKOP_ABC(op,a,b,c)  (MKOP_AB(op,a,b)|MKARG_C(c))
#define MKOP_ABx(op,a,bx)   (MKOP_A(op,a)|MKARG_Bx(bx))
#define MKOP_Bx(op,bx)      (MKOPCODE(op)|MKARG_Bx(bx))
#define MKOP_sBx(op,sbx)    (MKOPCODE(op)|MKARG_sBx(sbx))
#define MKOP_AsBx(op,a,sbx) (MKOP_A(op,a)|MKARG_sBx(sbx))
#define MKOP_Ax(op,ax)      (MKOPCODE(op)|MKARG_Ax(ax))
#define MKOP_Abc(op,a,b,c)  (MKOP_A(op,a)|MKARG_bc(b,c))

enum {
OP_NOP=0,/*                                                             */
OP_MOVE,/*      A B     R(A) := R(B)                                    */
OP_LOADL,/*     A Bx    R(A) := Lit(Bx)                                 */
OP_LOADI,/*     A sBx   R(A) := sBx                                     */
OP_LOADSYM,/*   A Bx    R(A) := Sym(Bx)                                 */
OP_LOADNIL,/*   A       R(A) := nil                                     */
OP_LOADSELF,/*  A       R(A) := self                                    */
OP_LOADT,/*     A       R(A) := true                                    */
OP_LOADF,/*     A       R(A) := false                                   */

OP_GETGLOBAL,/* A Bx    R(A) := getglobal(Sym(Bx))                      */
OP_SETGLOBAL,/* A Bx    setglobal(Sym(Bx), R(A))                        */
OP_GETSPECIAL,/*A Bx    R(A) := Special[Bx]                             */
OP_SETSPECIAL,/*A Bx    Special[Bx] := R(A)                             */
OP_GETIV,/*     A Bx    R(A) := ivget(Sym(Bx))                          */
OP_SETIV,/*     A Bx    ivset(Sym(Bx),R(A))                             */
OP_GETCV,/*     A Bx    R(A) := cvget(Sym(Bx))                          */
OP_SETCV,/*     A Bx    cvset(Sym(Bx),R(A))                             */
OP_GETCONST,/*  A Bx    R(A) := constget(Sym(Bx))                       */
OP_SETCONST,/*  A Bx    constset(Sym(Bx),R(A))                          */
OP_GETMCNST,/*  A Bx    R(A) := R(A)::Sym(B)                            */
OP_SETMCNST,/*  A Bx    R(A+1)::Sym(B) := R(A)                          */
OP_GETUPVAR,/*  A B C   R(A) := uvget(B,C)                              */
OP_SETUPVAR,/*  A B C   uvset(B,C,R(A))                                 */

OP_JMP,/*       sBx     pc+=sBx                                         */
OP_JMPIF,/*     A sBx   if R(A) pc+=sBx                                 */
OP_JMPNOT,/*    A sBx   if !R(A) pc+=sBx                                */
OP_ONERR,/*     sBx     rescue_push(pc+sBx)                             */
OP_RESCUE,/*    A       clear(exc); R(A) := exception (ignore when A=0) */
OP_POPERR,/*    A       A.times{rescue_pop()}                           */
OP_RAISE,/*     A       raise(R(A))                                     */
OP_EPUSH,/*     Bx      ensure_push(SEQ[Bx])                            */
OP_EPOP,/*      A       A.times{ensure_pop().call}                      */

OP_SEND,/*      A B C   R(A) := call(R(A),mSym(B),R(A+1),...,R(A+C))    */
OP_SENDB,/*     A B C   R(A) := call(R(A),mSym(B),R(A+1),...,R(A+C),&R(A+C+1))*/
OP_FSEND,/*     A B C   R(A) := fcall(R(A),mSym(B),R(A+1),...,R(A+C-1)) */
OP_CALL,/*      A B C   R(A) := self.call(R(A),.., R(A+C))              */
OP_SUPER,/*     A B C   R(A) := super(R(A+1),... ,R(A+C-1))             */
OP_ARGARY,/*    A Bx    R(A) := argument array (16=6:1:5:4)             */
OP_ENTER,/*     Ax      arg setup according to flags (24=5:5:1:5:5:1:1) */
OP_KARG,/*      A B C   R(A) := kdict[mSym(B)]; if C kdict.rm(mSym(B))  */
OP_KDICT,/*     A C     R(A) := kdict                                   */

OP_RETURN,/*    A B     return R(A) (B=normal,in-block return/break)    */
OP_TAILCALL,/*  A B C   return call(R(A),mSym(B),*R(C))                 */
OP_BLKPUSH,/*   A Bx    R(A) := block (16=6:1:5:4)                      */

OP_ADD,/*       A B C   R(A) := R(A)+R(A+1) (mSyms[B]=:+,C=1)           */
OP_ADDI,/*      A B C   R(A) := R(A)+C (mSyms[B]=:+)                    */
OP_SUB,/*       A B C   R(A) := R(A)-R(A+1) (mSyms[B]=:-,C=1)           */
OP_SUBI,/*      A B C   R(A) := R(A)-C (mSyms[B]=:-)                    */
OP_MUL,/*       A B C   R(A) := R(A)*R(A+1) (mSyms[B]=:*,C=1)           */
OP_DIV,/*       A B C   R(A) := R(A)/R(A+1) (mSyms[B]=:/,C=1)           */
OP_EQ,/*        A B C   R(A) := R(A)==R(A+1) (mSyms[B]=:==,C=1)         */
OP_LT,/*        A B C   R(A) := R(A)<R(A+1)  (mSyms[B]=:<,C=1)          */
OP_LE,/*        A B C   R(A) := R(A)<=R(A+1) (mSyms[B]=:<=,C=1)         */
OP_GT,/*        A B C   R(A) := R(A)>R(A+1)  (mSyms[B]=:>,C=1)          */
OP_GE,/*        A B C   R(A) := R(A)>=R(A+1) (mSyms[B]=:>=,C=1)         */

OP_ARRAY,/*     A B C   R(A) := ary_new(R(B),R(B+1)..R(B+C))            */
OP_ARYCAT,/*    A B     ary_cat(R(A),R(B))                              */
OP_ARYPUSH,/*   A B     ary_push(R(A),R(B))                             */
OP_AREF,/*      A B C   R(A) := R(B)[C]                                 */
OP_ASET,/*      A B C   R(B)[C] := R(A)                                 */
OP_APOST,/*     A B C   *R(A),R(A+1)..R(A+C) := R(A)                    */

OP_STRING,/*    A Bx    R(A) := str_dup(Lit(Bx))                        */
OP_STRCAT,/*    A B     str_cat(R(A),R(B))                              */

OP_HASH,/*      A B C   R(A) := hash_new(R(B),R(B+1)..R(B+C))           */
OP_LAMBDA,/*    A Bz Cz R(A) := lambda(SEQ[Bz],Cm)                      */
OP_RANGE,/*     A B C   R(A) := range_new(R(B),R(B+1),C)                */

OP_OCLASS,/*    A       R(A) := ::Object                                */
OP_CLASS,/*     A B     R(A) := newclass(R(A),mSym(B),R(A+1))           */
OP_MODULE,/*    A B     R(A) := newmodule(R(A),mSym(B))                 */
OP_EXEC,/*      A Bx    R(A) := blockexec(R(A),SEQ[Bx])                 */
OP_METHOD,/*    A B     R(A).newmethod(mSym(B),R(A+1))                  */
OP_SCLASS,/*    A B     R(A) := R(B).singleton_class                    */
OP_TCLASS,/*    A       R(A) := target_class                            */

OP_DEBUG,/*     A       print R(A)                                      */
OP_STOP,/*              stop VM                                         */
OP_ERR,/*       Bx      raise RuntimeError with message Lit(Bx)         */

OP_RSVD1,/*             reserved instruction #1                         */
OP_RSVD2,/*             reserved instruction #2                         */
OP_RSVD3,/*             reserved instruction #3                         */
OP_RSVD4,/*             reserved instruction #4                         */
OP_RSVD5,/*             reserved instruction #5                         */
};

#define OP_L_STRICT  1
#define OP_L_CAPTURE 2
#define OP_L_METHOD  OP_L_STRICT
#define OP_L_LAMBDA  (OP_L_STRICT|OP_L_CAPTURE)
#define OP_L_BLOCK   OP_L_CAPTURE

#define OP_R_NORMAL 0
#define OP_R_BREAK  1
#define OP_R_RETURN 2

#endif  /* OPCODE_H */
