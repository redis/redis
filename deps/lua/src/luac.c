/*
** $Id: luac.c $
** Lua compiler (saves bytecodes to files; also lists bytecodes)
** See Copyright Notice in lua.h
*/

#define luac_c
#define LUA_CORE

#include "lprefix.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "ldebug.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lopnames.h"
#include "lstate.h"
#include "lundump.h"

static void PrintFunction(const Proto* f, int full);
#define luaU_print	PrintFunction

#define PROGNAME	"luac"		/* default program name */
#define OUTPUT		PROGNAME ".out"	/* default output file */

static int listing=0;			/* list bytecodes? */
static int dumping=1;			/* dump bytecodes? */
static int stripping=0;			/* strip debug information? */
static char Output[]={ OUTPUT };	/* default output file name */
static const char* output=Output;	/* actual output file name */
static const char* progname=PROGNAME;	/* actual program name */
static TString **tmname;

static void fatal(const char* message)
{
 fprintf(stderr,"%s: %s\n",progname,message);
 exit(EXIT_FAILURE);
}

static void cannot(const char* what)
{
 fprintf(stderr,"%s: cannot %s %s: %s\n",progname,what,output,strerror(errno));
 exit(EXIT_FAILURE);
}

static void usage(const char* message)
{
 if (*message=='-')
  fprintf(stderr,"%s: unrecognized option '%s'\n",progname,message);
 else
  fprintf(stderr,"%s: %s\n",progname,message);
 fprintf(stderr,
  "usage: %s [options] [filenames]\n"
  "Available options are:\n"
  "  -l       list (use -l -l for full listing)\n"
  "  -o name  output to file 'name' (default is \"%s\")\n"
  "  -p       parse only\n"
  "  -s       strip debug information\n"
  "  -v       show version information\n"
  "  --       stop handling options\n"
  "  -        stop handling options and process stdin\n"
  ,progname,Output);
 exit(EXIT_FAILURE);
}

#define IS(s)	(strcmp(argv[i],s)==0)

static int doargs(int argc, char* argv[])
{
 int i;
 int version=0;
 if (argv[0]!=NULL && *argv[0]!=0) progname=argv[0];
 for (i=1; i<argc; i++)
 {
  if (*argv[i]!='-')			/* end of options; keep it */
   break;
  else if (IS("--"))			/* end of options; skip it */
  {
   ++i;
   if (version) ++version;
   break;
  }
  else if (IS("-"))			/* end of options; use stdin */
   break;
  else if (IS("-l"))			/* list */
   ++listing;
  else if (IS("-o"))			/* output file */
  {
   output=argv[++i];
   if (output==NULL || *output==0 || (*output=='-' && output[1]!=0))
    usage("'-o' needs argument");
   if (IS("-")) output=NULL;
  }
  else if (IS("-p"))			/* parse only */
   dumping=0;
  else if (IS("-s"))			/* strip debug information */
   stripping=1;
  else if (IS("-v"))			/* show version */
   ++version;
  else					/* unknown option */
   usage(argv[i]);
 }
 if (i==argc && (listing || !dumping))
 {
  dumping=0;
  argv[--i]=Output;
 }
 if (version)
 {
  printf("%s\n",LUA_COPYRIGHT);
  if (version==argc-1) exit(EXIT_SUCCESS);
 }
 return i;
}

#define FUNCTION "(function()end)();"

static const char* reader(lua_State* L, void* ud, size_t* size)
{
 UNUSED(L);
 if ((*(int*)ud)--)
 {
  *size=sizeof(FUNCTION)-1;
  return FUNCTION;
 }
 else
 {
  *size=0;
  return NULL;
 }
}

#define toproto(L,i) getproto(s2v(L->top+(i)))

static const Proto* combine(lua_State* L, int n)
{
 if (n==1)
  return toproto(L,-1);
 else
 {
  Proto* f;
  int i=n;
  if (lua_load(L,reader,&i,"=(" PROGNAME ")",NULL)!=LUA_OK) fatal(lua_tostring(L,-1));
  f=toproto(L,-1);
  for (i=0; i<n; i++)
  {
   f->p[i]=toproto(L,i-n-1);
   if (f->p[i]->sizeupvalues>0) f->p[i]->upvalues[0].instack=0;
  }
  f->sizelineinfo=0;
  return f;
 }
}

static int writer(lua_State* L, const void* p, size_t size, void* u)
{
 UNUSED(L);
 return (fwrite(p,size,1,(FILE*)u)!=1) && (size!=0);
}

static int pmain(lua_State* L)
{
 int argc=(int)lua_tointeger(L,1);
 char** argv=(char**)lua_touserdata(L,2);
 const Proto* f;
 int i;
 tmname=G(L)->tmname;
 if (!lua_checkstack(L,argc)) fatal("too many input files");
 for (i=0; i<argc; i++)
 {
  const char* filename=IS("-") ? NULL : argv[i];
  if (luaL_loadfile(L,filename)!=LUA_OK) fatal(lua_tostring(L,-1));
 }
 f=combine(L,argc);
 if (listing) luaU_print(f,listing>1);
 if (dumping)
 {
  FILE* D= (output==NULL) ? stdout : fopen(output,"wb");
  if (D==NULL) cannot("open");
  lua_lock(L);
  luaU_dump(L,f,writer,D,stripping);
  lua_unlock(L);
  if (ferror(D)) cannot("write");
  if (fclose(D)) cannot("close");
 }
 return 0;
}

int main(int argc, char* argv[])
{
 lua_State* L;
 int i=doargs(argc,argv);
 argc-=i; argv+=i;
 if (argc<=0) usage("no input files given");
 L=luaL_newstate();
 if (L==NULL) fatal("cannot create state: not enough memory");
 lua_pushcfunction(L,&pmain);
 lua_pushinteger(L,argc);
 lua_pushlightuserdata(L,argv);
 if (lua_pcall(L,2,0,0)!=LUA_OK) fatal(lua_tostring(L,-1));
 lua_close(L);
 return EXIT_SUCCESS;
}

/*
** print bytecodes
*/

#define UPVALNAME(x) ((f->upvalues[x].name) ? getstr(f->upvalues[x].name) : "-")
#define VOID(p) ((const void*)(p))
#define eventname(i) (getstr(tmname[i]))

static void PrintString(const TString* ts)
{
 const char* s=getstr(ts);
 size_t i,n=tsslen(ts);
 printf("\"");
 for (i=0; i<n; i++)
 {
  int c=(int)(unsigned char)s[i];
  switch (c)
  {
   case '"':
	printf("\\\"");
	break;
   case '\\':
	printf("\\\\");
	break;
   case '\a':
	printf("\\a");
	break;
   case '\b':
	printf("\\b");
	break;
   case '\f':
	printf("\\f");
	break;
   case '\n':
	printf("\\n");
	break;
   case '\r':
	printf("\\r");
	break;
   case '\t':
	printf("\\t");
	break;
   case '\v':
	printf("\\v");
	break;
   default:
	if (isprint(c)) printf("%c",c); else printf("\\%03d",c);
	break;
  }
 }
 printf("\"");
}

static void PrintType(const Proto* f, int i)
{
 const TValue* o=&f->k[i];
 switch (ttypetag(o))
 {
  case LUA_VNIL:
	printf("N");
	break;
  case LUA_VFALSE:
  case LUA_VTRUE:
	printf("B");
	break;
  case LUA_VNUMFLT:
	printf("F");
	break;
  case LUA_VNUMINT:
	printf("I");
	break;
  case LUA_VSHRSTR:
  case LUA_VLNGSTR:
	printf("S");
	break;
  default:				/* cannot happen */
	printf("?%d",ttypetag(o));
	break;
 }
 printf("\t");
}

static void PrintConstant(const Proto* f, int i)
{
 const TValue* o=&f->k[i];
 switch (ttypetag(o))
 {
  case LUA_VNIL:
	printf("nil");
	break;
  case LUA_VFALSE:
	printf("false");
	break;
  case LUA_VTRUE:
	printf("true");
	break;
  case LUA_VNUMFLT:
	{
	char buff[100];
	sprintf(buff,LUA_NUMBER_FMT,fltvalue(o));
	printf("%s",buff);
	if (buff[strspn(buff,"-0123456789")]=='\0') printf(".0");
	break;
	}
  case LUA_VNUMINT:
	printf(LUA_INTEGER_FMT,ivalue(o));
	break;
  case LUA_VSHRSTR:
  case LUA_VLNGSTR:
	PrintString(tsvalue(o));
	break;
  default:				/* cannot happen */
	printf("?%d",ttypetag(o));
	break;
 }
}

#define COMMENT		"\t; "
#define EXTRAARG	GETARG_Ax(code[pc+1])
#define EXTRAARGC	(EXTRAARG*(MAXARG_C+1))
#define ISK		(isk ? "k" : "")

static void PrintCode(const Proto* f)
{
 const Instruction* code=f->code;
 int pc,n=f->sizecode;
 for (pc=0; pc<n; pc++)
 {
  Instruction i=code[pc];
  OpCode o=GET_OPCODE(i);
  int a=GETARG_A(i);
  int b=GETARG_B(i);
  int c=GETARG_C(i);
  int ax=GETARG_Ax(i);
  int bx=GETARG_Bx(i);
  int sb=GETARG_sB(i);
  int sc=GETARG_sC(i);
  int sbx=GETARG_sBx(i);
  int isk=GETARG_k(i);
  int line=luaG_getfuncline(f,pc);
  printf("\t%d\t",pc+1);
  if (line>0) printf("[%d]\t",line); else printf("[-]\t");
  printf("%-9s\t",opnames[o]);
  switch (o)
  {
   case OP_MOVE:
	printf("%d %d",a,b);
	break;
   case OP_LOADI:
	printf("%d %d",a,sbx);
	break;
   case OP_LOADF:
	printf("%d %d",a,sbx);
	break;
   case OP_LOADK:
	printf("%d %d",a,bx);
	printf(COMMENT); PrintConstant(f,bx);
	break;
   case OP_LOADKX:
	printf("%d",a);
	printf(COMMENT); PrintConstant(f,EXTRAARG);
	break;
   case OP_LOADFALSE:
	printf("%d",a);
	break;
   case OP_LFALSESKIP:
	printf("%d",a);
	break;
   case OP_LOADTRUE:
	printf("%d",a);
	break;
   case OP_LOADNIL:
	printf("%d %d",a,b);
	printf(COMMENT "%d out",b+1);
	break;
   case OP_GETUPVAL:
	printf("%d %d",a,b);
	printf(COMMENT "%s",UPVALNAME(b));
	break;
   case OP_SETUPVAL:
	printf("%d %d",a,b);
	printf(COMMENT "%s",UPVALNAME(b));
	break;
   case OP_GETTABUP:
	printf("%d %d %d",a,b,c);
	printf(COMMENT "%s",UPVALNAME(b));
	printf(" "); PrintConstant(f,c);
	break;
   case OP_GETTABLE:
	printf("%d %d %d",a,b,c);
	break;
   case OP_GETI:
	printf("%d %d %d",a,b,c);
	break;
   case OP_GETFIELD:
	printf("%d %d %d",a,b,c);
	printf(COMMENT); PrintConstant(f,c);
	break;
   case OP_SETTABUP:
	printf("%d %d %d%s",a,b,c,ISK);
	printf(COMMENT "%s",UPVALNAME(a));
	printf(" "); PrintConstant(f,b);
	if (isk) { printf(" "); PrintConstant(f,c); }
	break;
   case OP_SETTABLE:
	printf("%d %d %d%s",a,b,c,ISK);
	if (isk) { printf(COMMENT); PrintConstant(f,c); }
	break;
   case OP_SETI:
	printf("%d %d %d%s",a,b,c,ISK);
	if (isk) { printf(COMMENT); PrintConstant(f,c); }
	break;
   case OP_SETFIELD:
	printf("%d %d %d%s",a,b,c,ISK);
	printf(COMMENT); PrintConstant(f,b);
	if (isk) { printf(" "); PrintConstant(f,c); }
	break;
   case OP_NEWTABLE:
	printf("%d %d %d",a,b,c);
	printf(COMMENT "%d",c+EXTRAARGC);
	break;
   case OP_SELF:
	printf("%d %d %d%s",a,b,c,ISK);
	if (isk) { printf(COMMENT); PrintConstant(f,c); }
	break;
   case OP_ADDI:
	printf("%d %d %d",a,b,sc);
	break;
   case OP_ADDK:
	printf("%d %d %d",a,b,c);
	printf(COMMENT); PrintConstant(f,c);
	break;
   case OP_SUBK:
	printf("%d %d %d",a,b,c);
	printf(COMMENT); PrintConstant(f,c);
	break;
   case OP_MULK:
	printf("%d %d %d",a,b,c);
	printf(COMMENT); PrintConstant(f,c);
	break;
   case OP_MODK:
	printf("%d %d %d",a,b,c);
	printf(COMMENT); PrintConstant(f,c);
	break;
   case OP_POWK:
	printf("%d %d %d",a,b,c);
	printf(COMMENT); PrintConstant(f,c);
	break;
   case OP_DIVK:
	printf("%d %d %d",a,b,c);
	printf(COMMENT); PrintConstant(f,c);
	break;
   case OP_IDIVK:
	printf("%d %d %d",a,b,c);
	printf(COMMENT); PrintConstant(f,c);
	break;
   case OP_BANDK:
	printf("%d %d %d",a,b,c);
	printf(COMMENT); PrintConstant(f,c);
	break;
   case OP_BORK:
	printf("%d %d %d",a,b,c);
	printf(COMMENT); PrintConstant(f,c);
	break;
   case OP_BXORK:
	printf("%d %d %d",a,b,c);
	printf(COMMENT); PrintConstant(f,c);
	break;
   case OP_SHRI:
	printf("%d %d %d",a,b,sc);
	break;
   case OP_SHLI:
	printf("%d %d %d",a,b,sc);
	break;
   case OP_ADD:
	printf("%d %d %d",a,b,c);
	break;
   case OP_SUB:
	printf("%d %d %d",a,b,c);
	break;
   case OP_MUL:
	printf("%d %d %d",a,b,c);
	break;
   case OP_MOD:
	printf("%d %d %d",a,b,c);
	break;
   case OP_POW:
	printf("%d %d %d",a,b,c);
	break;
   case OP_DIV:
	printf("%d %d %d",a,b,c);
	break;
   case OP_IDIV:
	printf("%d %d %d",a,b,c);
	break;
   case OP_BAND:
	printf("%d %d %d",a,b,c);
	break;
   case OP_BOR:
	printf("%d %d %d",a,b,c);
	break;
   case OP_BXOR:
	printf("%d %d %d",a,b,c);
	break;
   case OP_SHL:
	printf("%d %d %d",a,b,c);
	break;
   case OP_SHR:
	printf("%d %d %d",a,b,c);
	break;
   case OP_MMBIN:
	printf("%d %d %d",a,b,c);
	printf(COMMENT "%s",eventname(c));
	break;
   case OP_MMBINI:
	printf("%d %d %d %d",a,sb,c,isk);
	printf(COMMENT "%s",eventname(c));
	if (isk) printf(" flip");
	break;
   case OP_MMBINK:
	printf("%d %d %d %d",a,b,c,isk);
	printf(COMMENT "%s ",eventname(c)); PrintConstant(f,b);
	if (isk) printf(" flip");
	break;
   case OP_UNM:
	printf("%d %d",a,b);
	break;
   case OP_BNOT:
	printf("%d %d",a,b);
	break;
   case OP_NOT:
	printf("%d %d",a,b);
	break;
   case OP_LEN:
	printf("%d %d",a,b);
	break;
   case OP_CONCAT:
	printf("%d %d",a,b);
	break;
   case OP_CLOSE:
	printf("%d",a);
	break;
   case OP_TBC:
	printf("%d",a);
	break;
   case OP_JMP:
	printf("%d",GETARG_sJ(i));
	printf(COMMENT "to %d",GETARG_sJ(i)+pc+2);
	break;
   case OP_EQ:
	printf("%d %d %d",a,b,isk);
	break;
   case OP_LT:
	printf("%d %d %d",a,b,isk);
	break;
   case OP_LE:
	printf("%d %d %d",a,b,isk);
	break;
   case OP_EQK:
	printf("%d %d %d",a,b,isk);
	printf(COMMENT); PrintConstant(f,b);
	break;
   case OP_EQI:
	printf("%d %d %d",a,sb,isk);
	break;
   case OP_LTI:
	printf("%d %d %d",a,sb,isk);
	break;
   case OP_LEI:
	printf("%d %d %d",a,sb,isk);
	break;
   case OP_GTI:
	printf("%d %d %d",a,sb,isk);
	break;
   case OP_GEI:
	printf("%d %d %d",a,sb,isk);
	break;
   case OP_TEST:
	printf("%d %d",a,isk);
	break;
   case OP_TESTSET:
	printf("%d %d %d",a,b,isk);
	break;
   case OP_CALL:
	printf("%d %d %d",a,b,c);
	printf(COMMENT);
	if (b==0) printf("all in "); else printf("%d in ",b-1);
	if (c==0) printf("all out"); else printf("%d out",c-1);
	break;
   case OP_TAILCALL:
	printf("%d %d %d",a,b,c);
	printf(COMMENT "%d in",b-1);
	break;
   case OP_RETURN:
	printf("%d %d %d",a,b,c);
	printf(COMMENT);
	if (b==0) printf("all out"); else printf("%d out",b-1);
	break;
   case OP_RETURN0:
	break;
   case OP_RETURN1:
	printf("%d",a);
	break;
   case OP_FORLOOP:
	printf("%d %d",a,bx);
	printf(COMMENT "to %d",pc-bx+2);
	break;
   case OP_FORPREP:
	printf("%d %d",a,bx);
	printf(COMMENT "to %d",pc+bx+2);
	break;
   case OP_TFORPREP:
	printf("%d %d",a,bx);
	printf(COMMENT "to %d",pc+bx+2);
	break;
   case OP_TFORCALL:
	printf("%d %d",a,c);
	break;
   case OP_TFORLOOP:
	printf("%d %d",a,bx);
	printf(COMMENT "to %d",pc-bx+2);
	break;
   case OP_SETLIST:
	printf("%d %d %d",a,b,c);
	if (isk) printf(COMMENT "%d",c+EXTRAARGC);
	break;
   case OP_CLOSURE:
	printf("%d %d",a,bx);
	printf(COMMENT "%p",VOID(f->p[bx]));
	break;
   case OP_VARARG:
	printf("%d %d",a,c);
	printf(COMMENT);
	if (c==0) printf("all out"); else printf("%d out",c-1);
	break;
   case OP_VARARGPREP:
	printf("%d",a);
	break;
   case OP_EXTRAARG:
	printf("%d",ax);
	break;
#if 0
   default:
	printf("%d %d %d",a,b,c);
	printf(COMMENT "not handled");
	break;
#endif
  }
  printf("\n");
 }
}


#define SS(x)	((x==1)?"":"s")
#define S(x)	(int)(x),SS(x)

static void PrintHeader(const Proto* f)
{
 const char* s=f->source ? getstr(f->source) : "=?";
 if (*s=='@' || *s=='=')
  s++;
 else if (*s==LUA_SIGNATURE[0])
  s="(bstring)";
 else
  s="(string)";
 printf("\n%s <%s:%d,%d> (%d instruction%s at %p)\n",
	(f->linedefined==0)?"main":"function",s,
	f->linedefined,f->lastlinedefined,
	S(f->sizecode),VOID(f));
 printf("%d%s param%s, %d slot%s, %d upvalue%s, ",
	(int)(f->numparams),f->is_vararg?"+":"",SS(f->numparams),
	S(f->maxstacksize),S(f->sizeupvalues));
 printf("%d local%s, %d constant%s, %d function%s\n",
	S(f->sizelocvars),S(f->sizek),S(f->sizep));
}

static void PrintDebug(const Proto* f)
{
 int i,n;
 n=f->sizek;
 printf("constants (%d) for %p:\n",n,VOID(f));
 for (i=0; i<n; i++)
 {
  printf("\t%d\t",i);
  PrintType(f,i);
  PrintConstant(f,i);
  printf("\n");
 }
 n=f->sizelocvars;
 printf("locals (%d) for %p:\n",n,VOID(f));
 for (i=0; i<n; i++)
 {
  printf("\t%d\t%s\t%d\t%d\n",
  i,getstr(f->locvars[i].varname),f->locvars[i].startpc+1,f->locvars[i].endpc+1);
 }
 n=f->sizeupvalues;
 printf("upvalues (%d) for %p:\n",n,VOID(f));
 for (i=0; i<n; i++)
 {
  printf("\t%d\t%s\t%d\t%d\n",
  i,UPVALNAME(i),f->upvalues[i].instack,f->upvalues[i].idx);
 }
}

static void PrintFunction(const Proto* f, int full)
{
 int i,n=f->sizep;
 PrintHeader(f);
 PrintCode(f);
 if (full) PrintDebug(f);
 for (i=0; i<n; i++) PrintFunction(f->p[i],full);
}
