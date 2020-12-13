/*
** $Id: ljumptab.h $
** Jump Table for the Lua interpreter
** See Copyright Notice in lua.h
*/


#undef vmdispatch
#undef vmcase
#undef vmbreak

#define vmdispatch(x)     goto *disptab[x];

#define vmcase(l)     L_##l:

#define vmbreak		vmfetch(); vmdispatch(GET_OPCODE(i));


static const void *const disptab[NUM_OPCODES] = {

#if 0
** you can update the following list with this command:
**
**  sed -n '/^OP_/\!d; s/OP_/\&\&L_OP_/ ; s/,.*/,/ ; s/\/.*// ; p'  lopcodes.h
**
#endif

&&L_OP_MOVE,
&&L_OP_LOADI,
&&L_OP_LOADF,
&&L_OP_LOADK,
&&L_OP_LOADKX,
&&L_OP_LOADFALSE,
&&L_OP_LFALSESKIP,
&&L_OP_LOADTRUE,
&&L_OP_LOADNIL,
&&L_OP_GETUPVAL,
&&L_OP_SETUPVAL,
&&L_OP_GETTABUP,
&&L_OP_GETTABLE,
&&L_OP_GETI,
&&L_OP_GETFIELD,
&&L_OP_SETTABUP,
&&L_OP_SETTABLE,
&&L_OP_SETI,
&&L_OP_SETFIELD,
&&L_OP_NEWTABLE,
&&L_OP_SELF,
&&L_OP_ADDI,
&&L_OP_ADDK,
&&L_OP_SUBK,
&&L_OP_MULK,
&&L_OP_MODK,
&&L_OP_POWK,
&&L_OP_DIVK,
&&L_OP_IDIVK,
&&L_OP_BANDK,
&&L_OP_BORK,
&&L_OP_BXORK,
&&L_OP_SHRI,
&&L_OP_SHLI,
&&L_OP_ADD,
&&L_OP_SUB,
&&L_OP_MUL,
&&L_OP_MOD,
&&L_OP_POW,
&&L_OP_DIV,
&&L_OP_IDIV,
&&L_OP_BAND,
&&L_OP_BOR,
&&L_OP_BXOR,
&&L_OP_SHL,
&&L_OP_SHR,
&&L_OP_MMBIN,
&&L_OP_MMBINI,
&&L_OP_MMBINK,
&&L_OP_UNM,
&&L_OP_BNOT,
&&L_OP_NOT,
&&L_OP_LEN,
&&L_OP_CONCAT,
&&L_OP_CLOSE,
&&L_OP_TBC,
&&L_OP_JMP,
&&L_OP_EQ,
&&L_OP_LT,
&&L_OP_LE,
&&L_OP_EQK,
&&L_OP_EQI,
&&L_OP_LTI,
&&L_OP_LEI,
&&L_OP_GTI,
&&L_OP_GEI,
&&L_OP_TEST,
&&L_OP_TESTSET,
&&L_OP_CALL,
&&L_OP_TAILCALL,
&&L_OP_RETURN,
&&L_OP_RETURN0,
&&L_OP_RETURN1,
&&L_OP_FORLOOP,
&&L_OP_FORPREP,
&&L_OP_TFORPREP,
&&L_OP_TFORCALL,
&&L_OP_TFORLOOP,
&&L_OP_SETLIST,
&&L_OP_CLOSURE,
&&L_OP_VARARG,
&&L_OP_VARARGPREP,
&&L_OP_EXTRAARG

};
