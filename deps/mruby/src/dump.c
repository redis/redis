/*
** dump.c - mruby binary dumper (Rite binary format)
**
** See Copyright Notice in mruby.h
*/

#include <string.h>
#include "mruby/dump.h"
#include <ctype.h>

#include "mruby/string.h"
#ifdef ENABLE_REGEXP
#include "re.h"
#endif
#include "mruby/irep.h"

static const unsigned char def_rite_binary_header[] =
  RITE_FILE_IDENFIFIER
  RITE_FILE_FORMAT_VER
  RITE_VM_VER
  RITE_COMPILER_TYPE
  RITE_COMPILER_VER
  "0000"     //Binary data size
  "00"       //Number of ireps
  "00"       //Start index
  RITE_RESERVED
;

static const unsigned char def_rite_file_header[] =
  RITE_FILE_IDENFIFIER
  RITE_FILE_FORMAT_VER
  RITE_VM_VER
  RITE_COMPILER_TYPE
  RITE_COMPILER_VER
  "00000000" //Binary data size
  "0000"     //Number of ireps
  "0000"     //Start index
  RITE_RESERVED
  "0000"     //CRC
;

const char bin2hex[] = {
  '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};

#define DUMP_SIZE(size, type) ((type == DUMP_TYPE_BIN) ? size : size * RITE_FILE_HEX_SIZE)

enum {
  DUMP_IREP_HEADER = 0,
  DUMP_ISEQ_BLOCK,
  DUMP_POOL_BLOCK,
  DUMP_SYMS_BLOCK,
  DUMP_SECTION_NUM,
};

uint16_t calc_crc_16_ccitt(unsigned char*,int);
static inline int uint8_dump(uint8_t,char*,int);
static inline int uint16_dump(uint16_t,char*,int);
static inline int uint32_dump(uint32_t,char*,int);
static char* str_dump(char*,char*,uint16_t,int);
static uint16_t str_dump_len(char*,uint16_t, int);
static uint32_t get_irep_header_size(mrb_state*,mrb_irep*,int);
static uint32_t get_iseq_block_size(mrb_state*,mrb_irep*,int);
static uint32_t get_pool_block_size(mrb_state*,mrb_irep*,int);
static uint32_t get_syms_block_size(mrb_state*,mrb_irep*,int);
static uint32_t get_irep_record_size(mrb_state*,int,int);
static int write_irep_header(mrb_state*,mrb_irep*,char*,int);
static int write_iseq_block(mrb_state*,mrb_irep*,char*,int);
static int write_pool_block(mrb_state*,mrb_irep*,char*,int);
static int write_syms_block(mrb_state*,mrb_irep*,char*,int);
static int calc_crc_section(mrb_state*,mrb_irep*,uint16_t*,int);
static int write_rite_header(mrb_state*,int,char*,uint32_t);
static int dump_rite_header(mrb_state*,int,FILE*,uint32_t);
static int write_irep_record(mrb_state*,int,char*,uint32_t*,int);
static int dump_irep_record(mrb_state*,int,FILE*,uint32_t*);
static int mrb_write_irep(mrb_state*,int,char*);


static inline int
uint8_dump(uint8_t bin, char *hex, int type)
{
  if (type == DUMP_TYPE_BIN) {
    *hex = bin;
  } else {
    *hex++  = bin2hex[(bin >> 4) & 0x0f];
    *hex    = bin2hex[bin & 0x0f];
  }
  return DUMP_SIZE(MRB_DUMP_SIZE_OF_CHAR, type);
}

static inline int
uint16_dump(uint16_t bin, char *hex, int type)
{
  if (type == DUMP_TYPE_BIN) {
    return (uint16_to_bin(bin, hex));
  } else {
    *hex++  = bin2hex[(bin >> 12)& 0x0f];
    *hex++  = bin2hex[(bin >> 8) & 0x0f];
    *hex++  = bin2hex[(bin >> 4) & 0x0f];
    *hex    = bin2hex[bin & 0x0f];
    return DUMP_SIZE(MRB_DUMP_SIZE_OF_SHORT, type);
  }
}

static inline int
uint32_dump(uint32_t bin, char *hex, int type)
{
  if (type == DUMP_TYPE_BIN) {
    return (uint32_to_bin(bin, hex));
  } else {
    *hex++  = bin2hex[(bin >> 28) & 0x0f];
    *hex++  = bin2hex[(bin >> 24) & 0x0f];
    *hex++  = bin2hex[(bin >> 20) & 0x0f];
    *hex++  = bin2hex[(bin >> 16) & 0x0f];
    *hex++  = bin2hex[(bin >> 12) & 0x0f];
    *hex++  = bin2hex[(bin >> 8)  & 0x0f];
    *hex++  = bin2hex[(bin >> 4)  & 0x0f];
    *hex    = bin2hex[bin & 0x0f];
    return DUMP_SIZE(MRB_DUMP_SIZE_OF_LONG, type);
  }
}

#define CHAR_ESC_LEN 13 /* sizeof(\x{ hex of 32bit unsigned int } \0) */

static char*
str_dump(char *str, char *hex, uint16_t len, int type)
{
  if (type == DUMP_TYPE_BIN)
    memcpy(hex, str, len);
  else {
    char *src, *dst, buf[CHAR_ESC_LEN + 1];
    int n;

    for (src = str, dst = hex; len > 0; src++, dst++, len--) {
      switch (*src) {
      case 0x07:/* BEL */ *dst++ = '\\'; *dst = 'a'; break;
      case 0x08:/* BS  */ *dst++ = '\\'; *dst = 'b'; break;
      case 0x09:/* HT  */ *dst++ = '\\'; *dst = 't'; break;
      case 0x0A:/* LF  */ *dst++ = '\\'; *dst = 'n'; break;
      case 0x0B:/* VT  */ *dst++ = '\\'; *dst = 'v'; break;
      case 0x0C:/* FF  */ *dst++ = '\\'; *dst = 'f'; break;
      case 0x0D:/* CR  */ *dst++ = '\\'; *dst = 'r'; break;
      case 0x5C:/* \   */ *dst++ = '\\'; *dst = '\\'; break;
      case 0x22:/* "   */ /* fall through */
      case 0x27:/* '   */ /* fall through */
  //  case 0x3F:/* ?   */ /* fall through */
      default:
        if (*src >= ' ' && *src <= '~') {
          *dst = *src;
        } else {
          n = sprintf(buf, "\\%03o", *src & 0377);
          memcpy(dst, buf, n);
          dst += (n-1);
        }
        break;
      }
    }
  }

  return hex;
}

static uint16_t
str_dump_len(char *str, uint16_t len, int type)
{
  uint16_t dump_len = 0;

  if (type == DUMP_TYPE_BIN)
    dump_len = len;
  else {
    char *src;

    for (src = str; len > 0; src++, len--) {
      switch (*src) {
      case 0x07:/* BEL */ /* fall through */
      case 0x08:/* BS  */ /* fall through */
      case 0x09:/* HT  */ /* fall through */
      case 0x0A:/* LF  */ /* fall through */
      case 0x0B:/* VT  */ /* fall through */
      case 0x0C:/* FF  */ /* fall through */
      case 0x0D:/* CR  */ /* fall through */
      case 0x5C:/* \   */ /* fall through */
        dump_len += 2;
        break;

      case 0x22:/* "   */ /* fall through */
      case 0x27:/* '   */ /* fall through */
  //  case 0x3F:/* ?   */ /* fall through */
      default:
        if (*src >= ' ' && *src <= '~') {
          dump_len++;
        } else {
          // dump_len += sprintf(buf, "\\%03o", *src & 0377);
          dump_len += 4;
        }
        break;
      }
    }
  }

  return dump_len;
}

static uint32_t
get_irep_header_size(mrb_state *mrb, mrb_irep *irep, int type)
{
  uint32_t size = 0;

  size += 2;
  size += DUMP_SIZE(MRB_DUMP_SIZE_OF_SHORT, type) * 4;

  return size;
}

static uint32_t
get_iseq_block_size(mrb_state *mrb, mrb_irep *irep, int type)
{
  uint32_t size = 0;

  size += MRB_DUMP_SIZE_OF_LONG; /* ilen */
  size += irep->ilen * MRB_DUMP_SIZE_OF_LONG; /* iseq(n) */
  size += MRB_DUMP_SIZE_OF_SHORT; /* crc */

  return DUMP_SIZE(size, type);
}

static uint32_t
get_pool_block_size(mrb_state *mrb, mrb_irep *irep, int type)
{
  uint32_t size = 0;
  int pool_no;
  mrb_value str;
  char buf[32];

  size += MRB_DUMP_SIZE_OF_LONG; /* plen */
  size += irep->plen; /* tt(n) */
  size += irep->plen * MRB_DUMP_SIZE_OF_SHORT; /* len(n) */
  size += MRB_DUMP_SIZE_OF_SHORT; /* crc */
  size = DUMP_SIZE(size, type);

  for (pool_no = 0; pool_no < irep->plen; pool_no++) {
    uint16_t nlen =0;
    int len;

    switch (mrb_type(irep->pool[pool_no])) {
    case MRB_TT_FIXNUM:
      len = mrb_int_to_str( buf, mrb_fixnum(irep->pool[pool_no]));
      size += (uint32_t)len;
      break;
    case MRB_TT_FLOAT:
      len = mrb_float_to_str( buf, mrb_float(irep->pool[pool_no]));
      size += (uint32_t)len;
      break;
    case MRB_TT_STRING:
      str = mrb_string_value( mrb, &irep->pool[pool_no]);
      nlen = str_dump_len(RSTRING_PTR(str), RSTRING_LEN(str), type);
      size += nlen;
      break;
#ifdef ENABLE_REGEXP
    case MRB_TT_REGEX:
      str = mrb_reg_to_s(mrb, irep->pool[pool_no]);
      nlen = str_dump_len(RSTRING_PTR(str), RSTRING_LEN(str), type);
      size += nlen;
      break;
#endif
    default:
      break;
    }
  }

  return size;
}

static uint32_t
get_syms_block_size(mrb_state *mrb, mrb_irep *irep, int type)
{
  uint32_t size = 0;
  int sym_no;

  size += MRB_DUMP_SIZE_OF_LONG; /* slen */
  size += MRB_DUMP_SIZE_OF_SHORT; /* crc */
  size = DUMP_SIZE(size, type);

  for (sym_no = 0; sym_no < irep->slen; sym_no++) {
    const char * name;
    uint16_t nlen =0;

    size += DUMP_SIZE(MRB_DUMP_SIZE_OF_SHORT, type); /* snl(n) */
    if (irep->syms[sym_no] != 0) {
      int len;

      name = mrb_sym2name_len(mrb, irep->syms[sym_no], &len);
      nlen = str_dump_len((char*)name, len, type);
      size += nlen; /* sn(n) */
    }
  }

  return size;
}

static uint32_t
get_irep_record_size(mrb_state *mrb, int irep_no, int type)
{
  uint32_t size = 0;
  mrb_irep *irep = mrb->irep[irep_no];

  size += DUMP_SIZE(MRB_DUMP_SIZE_OF_LONG, type); /* rlen */
  size += get_irep_header_size(mrb, irep, type);
  size += get_iseq_block_size(mrb, irep, type);
  size += get_pool_block_size(mrb, irep, type);
  size += get_syms_block_size(mrb, irep, type);

  return size;
}

static int
write_irep_header(mrb_state *mrb, mrb_irep *irep, char *buf, int type)
{
  char *buf_top = buf;

  *buf++ = RITE_IREP_IDENFIFIER; /* record identifier */
  *buf++ = RITE_IREP_TYPE_CLASS; /* class or module */
  buf += uint16_dump((uint16_t)irep->nlocals, buf, type);  /* number of local variable */
  buf += uint16_dump((uint16_t)irep->nregs, buf, type);  /* number of register variable */
  buf += uint16_dump(DUMP_SIZE(MRB_DUMP_SIZE_OF_SHORT, type)/* crc */, buf, type); /* offset of isec block */

  return (int)(buf - buf_top);
}

static int
write_iseq_block(mrb_state *mrb, mrb_irep *irep, char *buf, int type)
{
  char *buf_top = buf;
  int iseq_no;

  buf += uint32_dump((uint32_t)irep->ilen, buf, type); /* number of opcode */

  for (iseq_no = 0; iseq_no < irep->ilen; iseq_no++) {
    buf += uint32_dump((uint32_t)irep->iseq[iseq_no], buf, type); /* opcode */
  }

  return (int)(buf - buf_top);
}

static int
write_pool_block(mrb_state *mrb, mrb_irep *irep, char *buf, int type)
{
  int pool_no;
  mrb_value str;
  char *buf_top = buf;
  char *char_buf;
  uint16_t buf_size =0;
  uint16_t len =0;

  buf_size = MRB_DUMP_DEFAULT_STR_LEN;
  if ((char_buf = (char *)mrb_malloc(mrb, buf_size)) == NULL)
    goto error_exit;

  buf += uint32_dump((uint32_t)irep->plen, buf, type); /* number of pool */

  for (pool_no = 0; pool_no < irep->plen; pool_no++) {
    buf += uint8_dump(mrb_type(irep->pool[pool_no]), buf, type); /* data type */
    memset(char_buf, 0, buf_size);

    switch (mrb_type(irep->pool[pool_no])) {
    case MRB_TT_FIXNUM:
      len = mrb_int_to_str(char_buf, mrb_fixnum(irep->pool[pool_no]));
      break;

    case MRB_TT_FLOAT:
      len = mrb_float_to_str(char_buf, mrb_float(irep->pool[pool_no]));
      break;

    case MRB_TT_STRING:
      str = irep->pool[pool_no];
      len = str_dump_len(RSTRING_PTR(str), RSTRING_LEN(str), type);
      if (len > buf_size - 1) {
        buf_size = len + 1;
        if ((char_buf = (char *)mrb_realloc(mrb, char_buf, buf_size)) == NULL)
          goto error_exit;
        memset(char_buf, 0, buf_size);
      }
      str_dump(RSTRING_PTR(str), char_buf, RSTRING_LEN(str), type);
      break;

#ifdef ENABLE_REGEXP
    case MRB_TT_REGEX:
      str = mrb_reg_to_s(mrb, irep->pool[pool_no]);
      len = str_dump_len(RSTRING_PTR(str), RSTRING_LEN(str), type);
      if ( len > buf_size - 1) {
        buf_size = len + 1;
        if ((char_buf = mrb_realloc(mrb, char_buf, buf_size)) == NULL)
          goto error_exit;
        memset(char_buf, 0, buf_size);
      }
      str_dump(RSTRING_PTR(str), char_buf, RSTRING_LEN(str), type);
      break;
#endif

    default:
      buf += uint16_dump(0, buf, type); /* data length = 0 */
      continue;
    }

    buf += uint16_dump(len, buf, type); /* data length */

    memcpy(buf, char_buf, len);
    buf += len;
  }

error_exit:
  if (char_buf)
    mrb_free(mrb, char_buf);
  return (int)(buf - buf_top);
}

static int
write_syms_block(mrb_state *mrb, mrb_irep *irep, char *buf, int type)
{
  int sym_no;
  char *buf_top = buf;
  char *char_buf;
  uint16_t buf_size =0;

  buf_size = MRB_DUMP_DEFAULT_STR_LEN;
  if ((char_buf = (char *)mrb_malloc(mrb, buf_size)) == NULL)
    goto error_exit;

  buf += uint32_dump((uint32_t)irep->slen, buf, type); /* number of symbol */

  for (sym_no = 0; sym_no < irep->slen; sym_no++) {
    const char * name;
    uint16_t nlen =0;

    if (irep->syms[sym_no] != 0) {
      int len;

      name = mrb_sym2name_len(mrb, irep->syms[sym_no], &len);
      nlen = str_dump_len((char*)name, len, type);
      if ( nlen > buf_size - 1) {
        buf_size = nlen + 1;
        if ((char_buf = (char *)mrb_realloc(mrb, char_buf, buf_size)) == NULL)
          goto error_exit;
      }
      memset(char_buf, 0, buf_size);
      str_dump((char*)name, char_buf, len, type);

      buf += uint16_dump(nlen, buf, type); /* length of symbol name */
      memcpy(buf, char_buf, nlen); /* symbol name */
      buf += nlen;
    }
    else {
      buf += uint16_dump(MRB_DUMP_NULL_SYM_LEN, buf, type); /* length of symbol name */
    }
  }

error_exit:
  if (char_buf)
    mrb_free(mrb, char_buf);
  return (int)(buf - buf_top);
}

static int
calc_crc_section(mrb_state *mrb, mrb_irep *irep, uint16_t *crc, int section)
{
  char *buf, *buf_top;
  uint32_t buf_size;
  int type = DUMP_TYPE_BIN;

  switch (section) {
  case DUMP_IREP_HEADER: buf_size = get_irep_header_size(mrb, irep, type); break;
  case DUMP_ISEQ_BLOCK:  buf_size = get_iseq_block_size(mrb, irep, type); break;
  case DUMP_POOL_BLOCK:  buf_size = get_pool_block_size(mrb, irep, type); break;
  case DUMP_SYMS_BLOCK:  buf_size = get_syms_block_size(mrb, irep, type); break;
  default: return MRB_DUMP_GENERAL_FAILURE;
  }

  if ((buf = (char *)mrb_calloc(mrb, 1, buf_size)) == NULL)
    return MRB_DUMP_GENERAL_FAILURE;

  buf_top = buf;

  switch (section) {
  case DUMP_IREP_HEADER: buf += write_irep_header(mrb, irep, buf, type); break;
  case DUMP_ISEQ_BLOCK: buf += write_iseq_block(mrb, irep, buf, type); break;
  case DUMP_POOL_BLOCK: buf += write_pool_block(mrb, irep, buf, type); break;
  case DUMP_SYMS_BLOCK: buf += write_syms_block(mrb, irep, buf, type); break;
  default: break;
  }

  *crc = calc_crc_16_ccitt((unsigned char*)buf_top, (int)(buf - buf_top));

  mrb_free(mrb, buf_top);

  return MRB_DUMP_OK;
}

static uint16_t
calc_rite_header_crc(mrb_state *mrb, int top, rite_binary_header *binary_header, uint32_t rbds, int type)
{
  memcpy( binary_header, def_rite_binary_header, sizeof(*binary_header));

  uint32_dump(rbds, (char*)binary_header->rbds, type);
  uint16_dump((uint16_t)mrb->irep_len, (char*)binary_header->nirep, type);
  uint16_dump((uint16_t)top, (char*)binary_header->sirep, type);

  return calc_crc_16_ccitt((unsigned char*)binary_header, sizeof(*binary_header));
}

static int
write_rite_header(mrb_state *mrb, int top, char* bin, uint32_t rbds)
{
  rite_binary_header *binary_header;
  uint16_t crc;
  int type = DUMP_TYPE_BIN;

  binary_header = (rite_binary_header*)bin;
  crc = calc_rite_header_crc(mrb, top, binary_header, rbds, type);
  bin += sizeof(*binary_header);
  uint16_dump(crc, bin, type);

  return MRB_DUMP_OK;
}

static int
calc_rite_file_header(mrb_state *mrb, int top, uint32_t rbds, rite_file_header *file_header)
{
  rite_binary_header *binary_header, b_header;
  uint16_t crc;
  int type;

  /* calc crc */
  type = DUMP_TYPE_BIN;
  binary_header = &b_header;
  crc = calc_rite_header_crc(mrb, top, binary_header, rbds, type);

  /* dump rbc header */
  memcpy( file_header, def_rite_file_header, sizeof(*file_header));

  type = DUMP_TYPE_HEX;
  uint32_dump(rbds, (char*)file_header->rbds, type);
  uint16_dump((uint16_t)mrb->irep_len, (char*)file_header->nirep, type);
  uint16_dump((uint16_t)top, (char*)file_header->sirep, type);
  uint16_dump(crc, (char*)file_header->hcrc, type);

  return MRB_DUMP_OK;
}

static int
dump_rite_header(mrb_state *mrb, int top, FILE* fp, uint32_t rbds)
{
  int rc = MRB_DUMP_OK;
  rite_file_header file_header;

  if (fseek(fp, 0, SEEK_SET) != 0)
    return MRB_DUMP_GENERAL_FAILURE;

  rc = calc_rite_file_header(mrb, top, rbds, &file_header);
  if (rc != MRB_DUMP_OK)
    return rc;

  if (fwrite(&file_header, sizeof(file_header), 1, fp) != 1)
    return MRB_DUMP_WRITE_FAULT;

  return MRB_DUMP_OK;
}

static int
write_irep_record(mrb_state *mrb, int irep_no, char* bin, uint32_t *rlen, int type)
{
  uint32_t irep_record_size;
  mrb_irep *irep = mrb->irep[irep_no];
  int section;

  if (irep == NULL)
    return MRB_DUMP_INVALID_IREP;

  /* buf alloc */
  irep_record_size = get_irep_record_size(mrb, irep_no, type);
  if (irep_record_size == 0)
    return MRB_DUMP_GENERAL_FAILURE;

  memset( bin, 0, irep_record_size);

  /* rlen */
  *rlen = irep_record_size - DUMP_SIZE(MRB_DUMP_SIZE_OF_LONG, type);

  bin += uint32_dump(*rlen, bin, type);

  for (section = 0; section < DUMP_SECTION_NUM; section++) {
    int rc;
    uint16_t crc;

    switch (section) {
    case DUMP_IREP_HEADER: bin += write_irep_header(mrb, irep, bin, type); break;
    case DUMP_ISEQ_BLOCK: bin += write_iseq_block(mrb, irep, bin, type); break;
    case DUMP_POOL_BLOCK: bin += write_pool_block(mrb, irep, bin, type); break;
    case DUMP_SYMS_BLOCK: bin += write_syms_block(mrb, irep, bin, type); break;
    default: break;
    }

    if ((rc = calc_crc_section(mrb, irep, &crc, section)) != 0)
      return rc;

    bin += uint16_dump(crc, bin, type); /* crc */
  }

  return MRB_DUMP_OK;
}

static int
dump_irep_record(mrb_state *mrb, int irep_no, FILE* fp, uint32_t *rlen)
{
  int rc = MRB_DUMP_OK;
  uint32_t irep_record_size;
  char *buf;
  mrb_irep *irep = mrb->irep[irep_no];

  if (irep == NULL)
    return MRB_DUMP_INVALID_IREP;

  /* buf alloc */
  irep_record_size = get_irep_record_size(mrb, irep_no, DUMP_TYPE_HEX);
  if (irep_record_size == 0)
    return MRB_DUMP_GENERAL_FAILURE;

  if ((buf = (char *)mrb_calloc(mrb, 1, irep_record_size)) == NULL)
    return MRB_DUMP_GENERAL_FAILURE;

  if ((rc = write_irep_record(mrb, irep_no, buf, rlen, DUMP_TYPE_HEX)) != MRB_DUMP_OK) {
    rc = MRB_DUMP_GENERAL_FAILURE;
    goto error_exit;
  }


  if (fwrite(buf, irep_record_size, 1, fp) != 1)
    rc = MRB_DUMP_WRITE_FAULT;

error_exit:
  mrb_free(mrb, buf);

  return rc;
}

static int
mrb_write_irep(mrb_state *mrb, int top, char *bin)
{
  int rc;
  uint32_t rlen=0; /* size of irep record */
  int irep_no;
  char *bin_top;

  if (mrb == NULL || top < 0 || top >= mrb->irep_len || bin == NULL)
    return MRB_DUMP_INVALID_ARGUMENT;

  bin_top = bin;
  bin += sizeof(rite_binary_header) + MRB_DUMP_SIZE_OF_SHORT/* crc */;

  for (irep_no=top; irep_no<mrb->irep_len; irep_no++) {
    if ((rc = write_irep_record(mrb, irep_no, bin, &rlen, DUMP_TYPE_BIN)) != 0)
      return rc;

    bin += (rlen + DUMP_SIZE(MRB_DUMP_SIZE_OF_LONG, DUMP_TYPE_BIN));
  }

  bin += uint32_dump(0, bin, DUMP_TYPE_BIN); /* end of file */

  rc = write_rite_header(mrb, top, bin_top, (bin - bin_top));    //TODO: Remove top(SIREP)

  return rc;
}

int
mrb_dump_irep(mrb_state *mrb, int top, FILE* fp)
{
  int rc;
  uint32_t rbds=0; /* size of Rite Binary Data */
  uint32_t rlen=0; /* size of irep record */
  int irep_no;

  if (mrb == NULL || top < 0 || top >= mrb->irep_len || fp == NULL)
    return MRB_DUMP_INVALID_ARGUMENT;

  if (fwrite(&def_rite_file_header, sizeof(rite_file_header), 1, fp) != 1) /* dummy write */
    return MRB_DUMP_WRITE_FAULT;

  for (irep_no=top; irep_no<mrb->irep_len; irep_no++) {
    if ((rc = dump_irep_record(mrb, irep_no, fp, &rlen)) != 0)
      return rc;

    rbds += rlen;
  }

  if (fwrite("00000000"/* end of file */, 8, 1, fp) != 1)
    return MRB_DUMP_WRITE_FAULT;

  rc = dump_rite_header(mrb, top, fp, rbds);    //TODO: Remove top(SIREP)

  return rc;
}

int
mrb_bdump_irep(mrb_state *mrb, int n, FILE *f,const char *initname)
{
  int rc;
  int irep_no;
  char *buf;
  int buf_size = 0;
  int buf_idx = 0;

  if (mrb == NULL || n < 0 || n >= mrb->irep_len || f == NULL || initname == NULL)
    return -1;

  buf_size = sizeof(rite_binary_header) + MRB_DUMP_SIZE_OF_SHORT/* crc */;
  for (irep_no=n; irep_no<mrb->irep_len; irep_no++)
    buf_size += get_irep_record_size(mrb, irep_no, DUMP_TYPE_BIN);
  buf_size += MRB_DUMP_SIZE_OF_LONG; /* end of file */

  if ((buf = (char *)mrb_malloc(mrb, buf_size)) == NULL)
    return MRB_DUMP_GENERAL_FAILURE;

  rc = mrb_write_irep(mrb, n, buf);

  if (rc == MRB_DUMP_OK) {
    fprintf(f, "const char %s[] = {", initname);
    while (buf_idx < buf_size ) {
      if (buf_idx % 16 == 0 ) fputs("\n", f);
      fprintf(f, "0x%02x,", (unsigned char)buf[buf_idx++]);
    }
    fputs("\n};\n", f);
  }

  mrb_free(mrb, buf);

  return rc;
}
