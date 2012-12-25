/*
** mruby/dump.h - mruby binary dumper (Rite binary format)
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBY_DUMP_H
#define MRUBY_DUMP_H

#if defined(__cplusplus)
extern "C" {
#endif

#include "mruby.h"
#include <stdio.h>
#include <stdint.h>

int mrb_dump_irep(mrb_state*,int,FILE*);
int mrb_read_irep(mrb_state*,const char*);
int mrb_read_irep_file(mrb_state*,FILE*);
/* mrb_value mrb_load_irep(mrb_state*,const char*); */ /* declared in <irep.h> */
mrb_value mrb_load_irep_file(mrb_state*,FILE*);

int mrb_bdump_irep(mrb_state *mrb, int n, FILE *f,const char *initname);

/* dump type */
#define DUMP_TYPE_CODE 0
#define DUMP_TYPE_BIN  1
#define DUMP_TYPE_HEX  2

/* dump/load error code */
#define MRB_DUMP_OK                     0
#define MRB_DUMP_GENERAL_FAILURE        -1
#define MRB_DUMP_WRITE_FAULT            -2
#define MRB_DUMP_READ_FAULT             -3
#define MRB_DUMP_CRC_ERROR              -4
#define MRB_DUMP_INVALID_FILE_HEADER    -5
#define MRB_DUMP_INVALID_IREP           -6
#define MRB_DUMP_INVALID_ARGUMENT       -7

/* size of long/int/short value on dump/load */
#define MRB_DUMP_SIZE_OF_LONG          4
#define MRB_DUMP_SIZE_OF_INT           4
#define MRB_DUMP_SIZE_OF_SHORT         2
#define MRB_DUMP_SIZE_OF_CHAR          1

/* null symbol length */
#define MRB_DUMP_NULL_SYM_LEN          0xFFFF

/* Use HEX format string */
#define RITE_FILE_IS_HEX

#ifdef RITE_FILE_IS_HEX
#define RITE_FILE_HEX_SIZE             2
#else
#define RITE_FILE_HEX_SIZE             1
#endif

/* Rite Binary File header */
#define RITE_FILE_IDENFIFIER           "RITE"
#define RITE_FILE_FORMAT_VER           "00090000"
#define RITE_VM_VER                    "00090000"
#define RITE_COMPILER_TYPE             "MATZ    "
#define RITE_COMPILER_VER              "00090000"
#define RITE_RESERVED                  "        "

/* irep header */
#define RITE_IREP_IDENFIFIER           'S'
#define RITE_IREP_TYPE_CLASS           'C'
#define RITE_IREP_TYPE_MODULE          'M'

#define MRB_DUMP_DEFAULT_STR_LEN       128

//Rite Binary file_header
typedef struct _rite_binary_header {
  unsigned char    rbfi[4];        //Rite Binary File Identify
  unsigned char    rbfv[8];        //Rite Binary File Format Version
  unsigned char    risv[8];        //Rite Instruction Specification Version
  unsigned char    rct[8];         //Rite Compiler Type
  unsigned char    rcv[8];         //Rite Compiler Version
  unsigned char    rbds[4];        //Rite Binary Data Size
  unsigned char    nirep[2];       //Number of ireps
  unsigned char    sirep[2];       //Start index
  unsigned char    rsv[8];         //Reserved
} rite_binary_header;

// Rite File file_header
typedef struct _rite_file_header {
  unsigned char    rbfi[4];        //Rite Binary File Identify
  unsigned char    rbfv[8];        //Rite Binary File Format Version
  unsigned char    risv[8];        //Rite Instruction Specification Version
  unsigned char    rct[8];         //Rite Compiler Type
  unsigned char    rcv[8];         //Rite Compiler Version
  unsigned char    rbds[8];        //Rite Binary Data Size
  unsigned char    nirep[4];       //Number of ireps
  unsigned char    sirep[4];       //Start index
  unsigned char    rsv[8];         //Reserved
  unsigned char    hcrc[4];        //HCRC
} rite_file_header;

static inline int
uint16_to_bin(uint16_t s, char *bin)
{
  *bin++ = (s >> 8) & 0xff;
  *bin   = s & 0xff;
  return (MRB_DUMP_SIZE_OF_SHORT);
}

static inline int
uint32_to_bin(uint32_t l, char *bin)
{
  *bin++ = (l >> 24) & 0xff;
  *bin++ = (l >> 16) & 0xff;
  *bin++ = (l >> 8) & 0xff;
  *bin   = l & 0xff;
  return (MRB_DUMP_SIZE_OF_LONG);
}

static inline uint32_t
bin_to_uint32(unsigned char bin[])
{
  return (uint32_t)bin[0] << 24 |
         (uint32_t)bin[1] << 16 |
         (uint32_t)bin[2] << 8  |
         (uint32_t)bin[3];
}

static inline uint16_t
bin_to_uint16(unsigned char bin[])
{
  return (uint16_t)bin[0] << 8 |
         (uint16_t)bin[1];
}

#if defined(__cplusplus)
}  /* extern "C" { */
#endif

#endif  /* MRUBY_DUMP_H */
