/*
** load.c - mruby binary loader
**
** See Copyright Notice in mruby.h
*/

#include <string.h>
#include "mruby/dump.h"

#include "mruby/string.h"
#ifdef ENABLE_REGEXP
#include "re.h"
#endif
#include "mruby/proc.h"
#include "mruby/irep.h"

typedef struct _RiteFILE
{
  FILE* fp;
  unsigned char buf[256];
  int cnt;
  int readlen;
} RiteFILE;

const char hex2bin[256] = {
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  //00-0f
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  //10-1f
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  //20-2f
  0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  0,  0,  0,  0,  0,  0,  //30-3f
  0, 10, 11, 12, 13, 14, 15,  0,  0,  0,  0,  0,  0,  0,  0,  0,  //40-4f
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  //50-5f
  0, 10, 11, 12, 13, 14, 15,  0,  0,  0,  0,  0,  0,  0,  0,  0   //60-6f
  //70-ff
};

static uint16_t hex_to_bin8(unsigned char*,unsigned char*);
static uint16_t hex_to_bin16(unsigned char*,unsigned char*);
static uint16_t hex_to_bin32(unsigned char*,unsigned char*);
static uint8_t hex_to_uint8(unsigned char*);
static uint16_t hex_to_uint16(unsigned char*);
static uint32_t hex_to_uint32(unsigned char*);
static char* hex_to_str(char*,char*,uint16_t*);
uint16_t calc_crc_16_ccitt(unsigned char*,int);
static unsigned char rite_fgetcSub(RiteFILE*);
static unsigned char rite_fgetc(RiteFILE*,int);
static unsigned char* rite_fgets(RiteFILE*,unsigned char*,int,int);
static int load_rite_header(FILE*,rite_binary_header*,unsigned char*);
static int load_rite_irep_record(mrb_state*, RiteFILE*,unsigned char*,uint32_t*);
static int read_rite_header(mrb_state*,unsigned char*,rite_binary_header*);
static int read_rite_irep_record(mrb_state*,unsigned char*,uint32_t*);


static unsigned char
rite_fgetcSub(RiteFILE* rfp)
{
  //only first call
  if (rfp->buf[0] == '\0') {
    rfp->readlen = fread(rfp->buf, 1, sizeof(rfp->buf), rfp->fp);
    rfp->cnt = 0;
  }

  if (rfp->readlen == rfp->cnt) {
    rfp->readlen = fread(rfp->buf, 1, sizeof(rfp->buf), rfp->fp);
    rfp->cnt = 0;
    if (rfp->readlen == 0) {
      return '\0';
    }
  }
  return rfp->buf[(rfp->cnt)++];
}

static unsigned char
rite_fgetc(RiteFILE* rfp, int ignorecomment)
{
  unsigned char  tmp;

  for (;;) {
    tmp = rite_fgetcSub(rfp);
    if (tmp == '\n' || tmp == '\r') {
      continue;
    }
    else if (ignorecomment && tmp == '#') {
      while (tmp != '\n' && tmp != '\r' && tmp != '\0')
        tmp = rite_fgetcSub(rfp);
      if (tmp == '\0')
        return '\0';
    }
    else {
      return tmp;
    }
  }
}

static unsigned char*
rite_fgets(RiteFILE* rfp, unsigned char* dst, int len, int ignorecomment)
{
  int i;

  for (i=0; i<len; i++) {
    if ('\0' == (dst[i] = rite_fgetc(rfp, ignorecomment))) {
      return NULL;
    }
  }
  return dst;
}

static int
load_rite_header(FILE* fp, rite_binary_header* bin_header, unsigned char* hcrc)
{
  rite_file_header    file_header;

  if (fread(&file_header, 1, sizeof(file_header), fp) < sizeof(file_header)) {
    return MRB_DUMP_READ_FAULT;
  }
  memcpy(bin_header->rbfi, file_header.rbfi, sizeof(file_header.rbfi));
  if (memcmp(bin_header->rbfi, RITE_FILE_IDENFIFIER, sizeof(bin_header->rbfi)) != 0) {
    return MRB_DUMP_INVALID_FILE_HEADER;    //File identifier error
  }
  memcpy(bin_header->rbfv, file_header.rbfv, sizeof(file_header.rbfv));
  if (memcmp(bin_header->rbfv, RITE_FILE_FORMAT_VER, sizeof(bin_header->rbfv)) != 0) {
    return MRB_DUMP_INVALID_FILE_HEADER;    //File format version error
  }
  memcpy(bin_header->risv, file_header.risv, sizeof(file_header.risv));
  memcpy(bin_header->rct, file_header.rct, sizeof(file_header.rct));
  memcpy(bin_header->rcv, file_header.rcv, sizeof(file_header.rcv));
  hex_to_bin32(bin_header->rbds, file_header.rbds);
  hex_to_bin16(bin_header->nirep, file_header.nirep);
  hex_to_bin16(bin_header->sirep, file_header.sirep);
  memcpy(bin_header->rsv, file_header.rsv, sizeof(file_header.rsv));
  memcpy(hcrc, file_header.hcrc, sizeof(file_header.hcrc));

  return MRB_DUMP_OK;
}

static int
load_rite_irep_record(mrb_state *mrb, RiteFILE* rfp, unsigned char* dst, uint32_t* len)
{
  int i;
  uint32_t blocklen;
  uint16_t offset, pdl, snl, clen;
  unsigned char hex2[2], hex4[4], hex8[8], hcrc[4];
  unsigned char *pStart;
  char *char_buf;
  uint16_t buf_size =0;

  buf_size = MRB_DUMP_DEFAULT_STR_LEN;
  if ((char_buf = (char *)mrb_malloc(mrb, buf_size)) == NULL)
    goto error_exit;

  pStart = dst;

  //IREP HEADER BLOCK
  *dst = rite_fgetc(rfp, TRUE);                         //record identifier
  if (*dst != RITE_IREP_IDENFIFIER)
    return MRB_DUMP_INVALID_IREP;
  dst += sizeof(unsigned char);
  *dst = rite_fgetc(rfp, TRUE);                         //class or module
  dst += sizeof(unsigned char);
  rite_fgets(rfp, hex4, sizeof(hex4), TRUE);            //number of local variable
  dst += hex_to_bin16(dst, hex4);
  rite_fgets(rfp, hex4, sizeof(hex4), TRUE);            //number of register variable
  dst += hex_to_bin16(dst, hex4);
  rite_fgets(rfp, hex4, sizeof(hex4), TRUE);            //offset of isec block
  offset = hex_to_uint16(hex4);
  rite_fgets(rfp, hcrc, sizeof(hcrc), TRUE);            //header CRC
  memset( char_buf, '\0', buf_size);
  rite_fgets(rfp, (unsigned char*)char_buf, (offset - (MRB_DUMP_SIZE_OF_SHORT * RITE_FILE_HEX_SIZE)), TRUE); //class or module name
  hex_to_str(char_buf, (char*)(dst + MRB_DUMP_SIZE_OF_SHORT + MRB_DUMP_SIZE_OF_SHORT), &clen); //class or module name
  dst += uint16_to_bin((MRB_DUMP_SIZE_OF_SHORT/*crc*/ + clen), (char*)dst); //offset of isec block
  dst += hex_to_bin16(dst, hcrc);                 //header CRC
  dst += clen;

  //ISEQ BLOCK
  rite_fgets(rfp, hex8, sizeof(hex8), TRUE);            //iseq length
  dst += hex_to_bin32(dst, hex8);
  blocklen = hex_to_uint32(hex8);
  for (i=0; i<blocklen; i++) {
    rite_fgets(rfp, hex8, sizeof(hex8), TRUE);          //iseq
    dst += hex_to_bin32(dst, hex8);
  }
  rite_fgets(rfp, hcrc, sizeof(hcrc), TRUE);            //iseq CRC
  dst += hex_to_bin16(dst, hcrc);

  //POOL BLOCK
  rite_fgets(rfp, hex8, sizeof(hex8), TRUE);            //pool length
  dst += hex_to_bin32(dst, hex8);
  blocklen = hex_to_uint32(hex8);
  for (i=0; i<blocklen; i++) {
    rite_fgets(rfp, hex2, sizeof(hex2), TRUE);          //TT
    dst += hex_to_bin8(dst, hex2);
    rite_fgets(rfp, hex4, sizeof(hex4), TRUE);          //pool data length
    pdl = hex_to_uint16(hex4);

    if ( pdl > buf_size - 1) {
      buf_size = pdl + 1;
      if ((char_buf = (char *)mrb_realloc(mrb, char_buf, buf_size)) == NULL)
        goto error_exit;
    }
    memset(char_buf, '\0', buf_size);
    rite_fgets(rfp, (unsigned char*)char_buf, pdl, FALSE); //pool
    hex_to_str(char_buf, (char*)(dst + MRB_DUMP_SIZE_OF_SHORT), &clen);
    dst += uint16_to_bin(clen, (char*)dst);
    dst += clen;
  }
  rite_fgets(rfp, hcrc, sizeof(hcrc), TRUE);            //pool CRC
  dst += hex_to_bin16(dst, hcrc);

  //SYMS BLOCK
  rite_fgets(rfp, hex8, sizeof(hex8), TRUE);            //syms length
  dst += hex_to_bin32(dst, hex8);
  blocklen = hex_to_uint32(hex8);
  for (i=0; i<blocklen; i++) {
    rite_fgets(rfp, hex4, sizeof(hex4), TRUE);          //symbol name length
    snl = hex_to_uint16(hex4);

    if (snl == MRB_DUMP_NULL_SYM_LEN) {
      dst += uint16_to_bin(snl, (char*)dst);
      continue;
    }

    if ( snl > buf_size - 1) {
      buf_size = snl + 1;
      if ((char_buf = (char *)mrb_realloc(mrb, char_buf, buf_size)) == NULL)
        goto error_exit;
    }
    memset(char_buf, '\0', buf_size);
    rite_fgets(rfp, (unsigned char*)char_buf, snl, FALSE); //symbol name
    hex_to_str(char_buf, (char*)(dst + MRB_DUMP_SIZE_OF_SHORT), &clen);
    dst += uint16_to_bin(clen, (char*)dst);
    dst += clen;
  }
  rite_fgets(rfp, hcrc, sizeof(hcrc), TRUE);            //syms CRC
  dst += hex_to_bin16(dst, hcrc);

  *len = dst - pStart;

error_exit:
  if (char_buf)
    mrb_free(mrb, char_buf);

  return MRB_DUMP_OK;
}

int
mrb_read_irep_file(mrb_state *mrb, FILE* fp)
{
  int ret, i;
  uint32_t  len, rlen = 0;
  unsigned char hex8[8], hcrc[4];
  unsigned char *dst, *rite_dst = NULL;
  rite_binary_header  bin_header;
  RiteFILE ritefp = { 0 };
  RiteFILE *rfp;

  if ((mrb == NULL) || (fp == NULL)) {
    return MRB_DUMP_INVALID_ARGUMENT;
  }
  ritefp.fp = fp;
  rfp = &ritefp;

  //Read File Header Section
  if ((ret = load_rite_header(fp, &bin_header, hcrc)) != MRB_DUMP_OK)
    return ret;

  len = sizeof(rite_binary_header) + bin_to_uint32(bin_header.rbds);
  if ((rite_dst = (unsigned char *)mrb_malloc(mrb, len)) == NULL)
    return MRB_DUMP_GENERAL_FAILURE;

  dst = rite_dst;
  memset(dst, 0x00, len);
  *(rite_binary_header *)dst = bin_header;
  dst += sizeof(rite_binary_header);
  dst += hex_to_bin16(dst, hcrc);

  //Read Binary Data Section
  len = bin_to_uint16(bin_header.nirep);
  for (i=0; i<len; i++) {
    rite_fgets(rfp, hex8, sizeof(hex8), TRUE);                      //record len
    dst += hex_to_bin32(dst, hex8);
    if ((ret = load_rite_irep_record(mrb, rfp, dst, &rlen)) != MRB_DUMP_OK) //irep info
      goto error_exit;
    dst += rlen;
  }
  rite_fgets(rfp, hex8, sizeof(hex8), TRUE);                        //dummy record len
  hex_to_bin32(dst, hex8);  /* dst += hex_to_bin32(dst, hex8); */
  if (0 != hex_to_uint32(hex8)) {
    ret = MRB_DUMP_INVALID_IREP;
    goto error_exit;
  }

  if (ret == MRB_DUMP_OK)
    ret = mrb_read_irep(mrb, (char*)rite_dst);

error_exit:
  if (rite_dst)
    mrb_free(mrb, rite_dst);

  return ret;
}

static int
read_rite_header(mrb_state *mrb, unsigned char *bin, rite_binary_header*  bin_header)
{
  uint16_t crc;

  *bin_header = *(rite_binary_header *)bin;
  bin += sizeof(rite_binary_header);
  if (memcmp(bin_header->rbfi, RITE_FILE_IDENFIFIER, sizeof(bin_header->rbfi)) != 0) {
    return MRB_DUMP_INVALID_FILE_HEADER;    //File identifier error
  }
  if (memcmp(bin_header->risv, RITE_VM_VER, sizeof(bin_header->risv)) != 0) {
    return MRB_DUMP_INVALID_FILE_HEADER;    //Instruction set version check
  }

  crc = calc_crc_16_ccitt((unsigned char*)bin_header, sizeof(*bin_header));   //Calculate CRC
  if (crc != bin_to_uint16(bin)) {
    return MRB_DUMP_INVALID_FILE_HEADER;    //CRC error
  }

  return bin_to_uint16(bin_header->nirep);
}

static int
read_rite_irep_record(mrb_state *mrb, unsigned char *src, uint32_t* len)
{
  int i, ret = MRB_DUMP_OK;
  char *buf;
  unsigned char *recordStart, *pStart;
  uint16_t crc, tt, pdl, snl, offset, bufsize=MRB_DUMP_DEFAULT_STR_LEN;
  mrb_int fix_num;
  mrb_float f;
  int plen;
  int ai = mrb_gc_arena_save(mrb);
  mrb_irep *irep = mrb_add_irep(mrb);

  recordStart = src;
  buf = (char *)mrb_malloc(mrb, bufsize);
  if (buf == NULL) {
    ret = MRB_DUMP_INVALID_IREP;
    goto error_exit;
  }

  //Header Section
  pStart = src;
  if (*src != RITE_IREP_IDENFIFIER)
    return MRB_DUMP_INVALID_IREP;
  src += (sizeof(unsigned char) * 2);
  irep->nlocals = bin_to_uint16(src);       //number of local variable
  src += MRB_DUMP_SIZE_OF_SHORT;
  irep->nregs = bin_to_uint16(src);         //number of register variable
  src += MRB_DUMP_SIZE_OF_SHORT;
  offset = bin_to_uint16(src);              //offset of isec block
  src += MRB_DUMP_SIZE_OF_SHORT;
  crc = calc_crc_16_ccitt(pStart, src - pStart);     //Calculate CRC
  if (crc != bin_to_uint16(src))             //header CRC
    return MRB_DUMP_INVALID_IREP;
  src += offset;

  //Binary Data Section
  //ISEQ BLOCK
  pStart = src;
  irep->ilen = bin_to_uint32(src);          //iseq length
  src += MRB_DUMP_SIZE_OF_LONG;
  if (irep->ilen > 0) {
    if ((irep->iseq = (mrb_code *)mrb_malloc(mrb, sizeof(mrb_code) * irep->ilen)) == NULL) {
      ret = MRB_DUMP_GENERAL_FAILURE;
      goto error_exit;
    }
    for (i=0; i<irep->ilen; i++) {
      irep->iseq[i] = bin_to_uint32(src);     //iseq
      src += MRB_DUMP_SIZE_OF_LONG;
    }
  }
  crc = calc_crc_16_ccitt((unsigned char*)pStart, src - pStart);     //Calculate CRC
  if (crc != bin_to_uint16(src)) {          //iseq CRC
    ret = MRB_DUMP_INVALID_IREP;
    goto error_exit;
  }
  src += MRB_DUMP_SIZE_OF_SHORT;

  //POOL BLOCK
  pStart = src;
  plen = bin_to_uint32(src);          //pool length
  src += MRB_DUMP_SIZE_OF_LONG;
  if (plen > 0) {
    irep->pool = (mrb_value *)mrb_malloc(mrb, sizeof(mrb_value) * plen);
    if (irep->pool == NULL) {
      ret = MRB_DUMP_INVALID_IREP;
      goto error_exit;
    }

    for (i=0; i<plen; i++) {
      tt = *src;                              //pool TT
      src += sizeof(unsigned char);
      pdl = bin_to_uint16(src);               //pool data length
      src += MRB_DUMP_SIZE_OF_SHORT;
      if (pdl > bufsize - 1) {
        mrb_free(mrb, buf);
        bufsize = pdl + 1;
        if ((buf = (char *)mrb_malloc(mrb, bufsize)) == NULL) {
          ret = MRB_DUMP_GENERAL_FAILURE;
          goto error_exit;
        }
      }
      memcpy(buf, src, pdl);
      src += pdl;
      buf[pdl] = '\0';

      switch (tt) {                           //pool data
      case MRB_TT_FIXNUM:
        fix_num = str_to_mrb_int(buf);
        irep->pool[i] = mrb_fixnum_value(fix_num);
        break;

      case MRB_TT_FLOAT:
        f = str_to_mrb_float(buf);
        irep->pool[i] = mrb_float_value(f);
        break;

      case MRB_TT_STRING:
        irep->pool[i] = mrb_str_new(mrb, buf, pdl);
        break;

#ifdef ENABLE_REGEXP
      case MRB_TT_REGEX:
        str = mrb_str_new(mrb, buf, pdl);
        irep->pool[i] = mrb_reg_quote(mrb, str);
        break;
#endif

      default:
        irep->pool[i] = mrb_nil_value();
        break;
      }
      irep->plen++;
      mrb_gc_arena_restore(mrb, ai);
    }
  }
  crc = calc_crc_16_ccitt((unsigned char*)pStart, src - pStart);     //Calculate CRC
  if (crc != bin_to_uint16(src)) {          //pool CRC
    ret = MRB_DUMP_INVALID_IREP;
    goto error_exit;
  }
  src += MRB_DUMP_SIZE_OF_SHORT;

  //SYMS BLOCK
  pStart = src;
  irep->slen = bin_to_uint32(src);          //syms length
  src += MRB_DUMP_SIZE_OF_LONG;
  if (irep->slen > 0) {
    if ((irep->syms = (mrb_sym *)mrb_malloc(mrb, sizeof(mrb_sym) * irep->slen)) == NULL) {
      ret = MRB_DUMP_INVALID_IREP;
      goto error_exit;
    }

    for (i = 0; i < irep->slen; i++) {
      static const mrb_sym mrb_sym_zero = { 0 };
      *irep->syms = mrb_sym_zero;
    }
    for (i=0; i<irep->slen; i++) {
      snl = bin_to_uint16(src);               //symbol name length
      src += MRB_DUMP_SIZE_OF_SHORT;

      if (snl == MRB_DUMP_NULL_SYM_LEN) {
        irep->syms[i] = 0;
        continue;
      }

      if (snl > bufsize - 1) {
        mrb_free(mrb, buf);
        bufsize = snl + 1;
        if ((buf = (char *)mrb_malloc(mrb, bufsize)) == NULL) {
          ret = MRB_DUMP_GENERAL_FAILURE;
          goto error_exit;
        }
      }
      memcpy(buf, src, snl);                  //symbol name
      src += snl;
      buf[snl] = '\0';
      irep->syms[i] = mrb_intern2(mrb, buf, snl);
    }
  }
  crc = calc_crc_16_ccitt((unsigned char*)pStart, src - pStart);     //Calculate CRC
  if (crc != bin_to_uint16(src)) {           //syms CRC
    ret = MRB_DUMP_INVALID_IREP;
    goto error_exit;
  }
  src += MRB_DUMP_SIZE_OF_SHORT;

  *len = src - recordStart;
error_exit:
  if (buf)
    mrb_free(mrb, buf);

  return ret;
}

int
mrb_read_irep(mrb_state *mrb, const char *bin)
{
  int ret = MRB_DUMP_OK, i, n, nirep, sirep;
  uint32_t len = 0;
  unsigned char *src;
  rite_binary_header  bin_header;

  if ((mrb == NULL) || (bin == NULL)) {
    return MRB_DUMP_INVALID_ARGUMENT;
  }
  src = (unsigned char*)bin;
  sirep = mrb->irep_len;

  //Read File Header Section
  if ((nirep = read_rite_header(mrb, src, &bin_header)) < 0)
    return nirep;
  
  src += sizeof(bin_header) + MRB_DUMP_SIZE_OF_SHORT;  //header + crc

  //Read Binary Data Section
  for (n=0,i=sirep; n<nirep; n++,i++) {
    src += MRB_DUMP_SIZE_OF_LONG;                      //record ren
    if ((ret = read_rite_irep_record(mrb, src, &len)) != MRB_DUMP_OK)
      goto error_exit;
    src += len;
  }
  if (0 != bin_to_uint32(src)) {              //dummy record len
    ret = MRB_DUMP_GENERAL_FAILURE;
  }

error_exit:
  if (ret != MRB_DUMP_OK) {
    for (n=0,i=sirep; i<mrb->irep_len; n++,i++) {
      if (mrb->irep[i]) {
        if (mrb->irep[i]->iseq)
          mrb_free(mrb, mrb->irep[i]->iseq);

        if (mrb->irep[i]->pool)
          mrb_free(mrb, mrb->irep[i]->pool);

        if (mrb->irep[i]->syms)
          mrb_free(mrb, mrb->irep[i]->syms);

        mrb_free(mrb, mrb->irep[i]);
      }
    }
    //    mrb->irep_len = sirep;
    return ret;
  }
  return sirep + hex_to_uint8(bin_header.sirep);
}

static uint16_t
hex_to_bin8(unsigned char *dst, unsigned char *src)
{
  dst[0] = (hex2bin[src[0]] << 4) | (hex2bin[src[1]]);
  return 1;
}

static uint16_t
hex_to_bin16(unsigned char *dst, unsigned char *src)
{
  dst[0] = (hex2bin[src[0]] << 4) | (hex2bin[src[1]]);
  dst[1] = (hex2bin[src[2]] << 4) | (hex2bin[src[3]]);
  return 2;
}

static uint16_t
hex_to_bin32(unsigned char *dst, unsigned char *src)
{
  dst[0] = (hex2bin[src[0]] << 4) | (hex2bin[src[1]]);
  dst[1] = (hex2bin[src[2]] << 4) | (hex2bin[src[3]]);
  dst[2] = (hex2bin[src[4]] << 4) | (hex2bin[src[5]]);
  dst[3] = (hex2bin[src[6]] << 4) | (hex2bin[src[7]]);
  return 4;
}

static uint8_t
hex_to_uint8(unsigned char *hex)
{
  return (unsigned char)hex2bin[hex[0]] << 4  |
         (unsigned char)hex2bin[hex[1]];
}

static uint16_t
hex_to_uint16(unsigned char *hex)
{
  return (uint16_t)hex2bin[hex[0]] << 12 |
         (uint16_t)hex2bin[hex[1]] << 8  |
         (uint16_t)hex2bin[hex[2]] << 4  |
         (uint16_t)hex2bin[hex[3]];
}

static uint32_t
hex_to_uint32(unsigned char *hex)
{
  return (uint32_t)hex2bin[hex[0]] << 28  |
         (uint32_t)hex2bin[hex[1]] << 24  |
         (uint32_t)hex2bin[hex[2]] << 20  |
         (uint32_t)hex2bin[hex[3]] << 16  |
         (uint32_t)hex2bin[hex[4]] << 12  |
         (uint32_t)hex2bin[hex[5]] << 8   |
         (uint32_t)hex2bin[hex[6]] << 4   |
         (uint32_t)hex2bin[hex[7]];
}

static char*
hex_to_str(char *hex, char *str, uint16_t *str_len)
{
  char *src, *dst, buf[4];
  int escape = 0, base = 0;
  char *err_ptr;

  *str_len = 0;
  for (src = hex, dst = str; *src != '\0'; src++) {
    if (escape) {
      switch(*src) {
      case 'a':  *dst++ = '\a'/* BEL */; break;
      case 'b':  *dst++ = '\b'/* BS  */; break;
      case 't':  *dst++ = '\t'/* HT  */; break;
      case 'n':  *dst++ = '\n'/* LF  */; break;
      case 'v':  *dst++ = '\v'/* VT  */; break;
      case 'f':  *dst++ = '\f'/* FF  */; break;
      case 'r':  *dst++ = '\r'/* CR  */; break;
      case '\"': /* fall through */
      case '\'': /* fall through */
      case '\?': /* fall through */
      case '\\': *dst++ = *src; break;
      default:
        if (*src >= '0' && *src <= '7') {
          base = 8;
          strncpy(buf, src, 3);
        } else if (*src == 'x' || *src == 'X') {
          base = 16;
          src++;
          strncpy(buf, src, 2);
        }

        *dst++ = (unsigned char) strtol(buf, &err_ptr, base) & 0xff;
        src += (err_ptr - buf - 1);
        break;
      }
      escape = 0;
    } else {
      if (*src == '\\') {
        escape = 1;
      } else {
        escape = 0;
        *dst++ = *src;
      }
    }
    if (!escape) {
      (*str_len)++;
    }
  }
  return str;
}

static void
irep_error(mrb_state *mrb, int n)
{
  static const char msg[] = "irep load error";
  mrb->exc = (struct RObject*)mrb_object(mrb_exc_new(mrb, E_SCRIPT_ERROR, msg, sizeof(msg) - 1));
}

mrb_value
mrb_load_irep_file(mrb_state *mrb, FILE* fp)
{
  int n = mrb_read_irep_file(mrb, fp);

  if (n < 0) {
    irep_error(mrb, n);
    return mrb_nil_value();
  }
  return mrb_run(mrb, mrb_proc_new(mrb, mrb->irep[n]), mrb_top_self(mrb));
}

mrb_value
mrb_load_irep(mrb_state *mrb, const char *bin)
{
  int n = mrb_read_irep(mrb, bin);

  if (n < 0) {
    irep_error(mrb, n);
    return mrb_nil_value();
  }
  return mrb_run(mrb, mrb_proc_new(mrb, mrb->irep[n]), mrb_top_self(mrb));
}
