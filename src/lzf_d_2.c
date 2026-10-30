/*
 * Copyright (c) 2000-2010 Marc Alexander Lehmann <schmorp@schmorp.de>
 * Modifications Copyright (c) 2024-Present, Redis Ltd., provided under the
 * same terms below.
 *
 * Redistribution and use in source and binary forms, with or without modifica-
 * tion, are permitted provided that the following conditions are met:
 *
 *   1.  Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *   2.  Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MER-
 * CHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPE-
 * CIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTH-
 * ERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Alternatively, the contents of this file may be used under the terms of
 * the GNU General Public License ("GPL") version 2 or any later version,
 * in which case the provisions of the GPL are applicable instead of
 * the above. If you wish to allow the use of your version of this file
 * only under the terms of the GPL and not to allow others to use your
 * version of this file under the BSD license, indicate your decision
 * by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL. If you do not delete the
 * provisions above, a recipient may use your version of this file under
 * either the BSD or the GPL.
 */

/*
 * lzf_decompress2(): an optimized, drop-in decoder derived from liblzf's
 * lzf_d.c. It consumes the exact same compressed format and is used by Redis
 * for RDB loading in place of lzf_decompress() (which remains in lzf_d.c and
 * is used as the reference/oracle by the fuzz test below).
 *
 * Why it is faster: the original decoder is dominated by branch
 * mispredictions from two computed-jump switch statements (a 32-way literal
 * "Duff's device" and a per-length back-reference switch) plus
 * variable-trip-count byte loops. This version uses an LZ4-style structure: a
 * branch-lean fast loop that copies in fixed 8/16/32 byte chunks while a whole
 * worst-case token is guaranteed to fit, and an exact byte-safe tail (the
 * original liblzf decode logic) that finishes the last few bytes with no
 * overshoot. The caller contract is unchanged: pass the exact uncompressed
 * length; no output slack is required.
 *
 * The small-offset (< 8) back-reference expansion below uses the technique and
 * constant tables from LZ4 (lz4.c), Copyright (c) Yann Collet, licensed under
 * the BSD 2-Clause License, which is compatible with the terms above.
 */

#include "lzfP.h"
#include "lzf.h"
#include "config.h"   /* for CACHE_LINE_SIZE */

#if AVOID_ERRNO
# define SET_ERRNO(n)
#else
# include <errno.h>
# define SET_ERRNO(n) errno = (n)
#endif

/* The fast loop writes ahead of the logical output position (a fixed 32-byte
 * literal copy, and 8-byte-granular back-reference copies), so it only runs
 * while a whole worst-case token still fits with room to spare. Once fewer
 * than LZFD_MARGIN output bytes remain, decoding falls back to the exact
 * byte-safe loop, which never writes past out_end.
 *
 * MARGIN must cover one maximal token: a 3-octet back-reference header can
 * expand to a 264-byte match, whose 8-byte-granular copy overshoots by up to
 * 7, plus the 32-byte literal store; 300 leaves headroom. */
#define LZFD_MARGIN 300

/* Tables to expand a back-reference with offset < 8 (a repeating pattern) into
 * the first 8 output bytes, then fix up the source pointer so an 8-byte-
 * granular copy can finish the rest without reading ahead of freshly written
 * data. These are LZ4's inc32table/dec64table (lz4.c, (c) Yann Collet,
 * BSD 2-Clause). */
static const unsigned lzfd_inc32[8] = { 0, 1, 2, 1, 0, 4, 4, 4 };
static const int      lzfd_dec64[8] = { 0, 0, 0, -1, -4, 1, 2, 3 };

/* Copy n (>0) bytes in 8-byte chunks, overshooting by up to 7. The caller
 * guarantees (dst - src) >= 8 so each 8-byte source chunk stays below the
 * current write position, and that >= 7 bytes of output slack remain. */
static inline void lzfd_wild_copy8 (u8 *d, const u8 *s, long n)
{
  do { memcpy (d, s, 8); d += 8; s += 8; n -= 8; } while (n > 0);
}

/* Pin the function to a cache line on x86-64 for stable code placement, matching
 * lzf_compress (see lzf_c.c). */
#if defined(__x86_64__)
__attribute__((aligned(CACHE_LINE_SIZE)))
#endif
size_t
lzf_decompress2 (const void *const in_data,  size_t in_len,
                 void             *out_data, size_t out_len)
{
  const u8 *ip = (const u8 *)in_data;
  u8       *op = (u8 *)out_data;
  const u8 *const in_end  = ip + in_len;
  u8       *const out_end = op + out_len;

  /* Fast path: only while a full worst-case token is guaranteed to fit. */
  if (in_len > 32 && out_len > LZFD_MARGIN)
    {
      const u8 *ip_lim = in_end  - 32;
      u8       *op_lim = out_end - LZFD_MARGIN;

      while (ip < ip_lim && op < op_lim)
        {
          unsigned int ctrl = *ip++;

          if (ctrl < (1 << 5)) /* literal run of 1..32 bytes */
            {
              ctrl++;
              memcpy (op,      ip,      16);
              memcpy (op + 16, ip + 16, 16);
              op += ctrl;
              ip += ctrl;
            }
          else /* back reference */
            {
              unsigned int len = ctrl >> 5;
              const u8 *ref = op - ((ctrl & 0x1f) << 8) - 1;

              if (len == 7)
                len += *ip++;

              ref -= *ip++;
              len += 2; /* total match length, 3..264 */

              if (ref < (u8 *)out_data)
                {
                  SET_ERRNO (EINVAL);
                  return 0;
                }

              {
                size_t off = (size_t) (op - ref);
                u8 *start = op;

                if (off >= 8)
                  {
                    memcpy (op, ref, 8);
                    ref += 8;
                  }
                else
                  {
                    /* offset < 8: materialise the repeating pattern branchlessly */
                    op[0] = ref[0]; op[1] = ref[1];
                    op[2] = ref[2]; op[3] = ref[3];
                    ref += lzfd_inc32[off];
                    memcpy (op + 4, ref, 4);
                    ref -= lzfd_dec64[off];
                  }

                op += 8;
                if (len > 8)
                  lzfd_wild_copy8 (op, ref, (long)len - 8);
                op = start + len;
              }
            }
        }
    }

  /* Exact, byte-safe tail. Finishes the last < LZFD_MARGIN bytes (or the whole
   * stream for tiny inputs) with full bounds checks and no overshoot. */
  while (ip < in_end)
    {
      unsigned int ctrl = *ip++;

      if (ctrl < (1 << 5)) /* literal run */
        {
          ctrl++;

          if (op + ctrl > out_end)
            {
              SET_ERRNO (E2BIG);
              return 0;
            }
#if CHECK_INPUT
          if (ip + ctrl > in_end)
            {
              SET_ERRNO (EINVAL);
              return 0;
            }
#endif
          do
            *op++ = *ip++;
          while (--ctrl);
        }
      else /* back reference */
        {
          unsigned int len = ctrl >> 5;
          u8 *ref = op - ((ctrl & 0x1f) << 8) - 1;

#if CHECK_INPUT
          if (ip >= in_end)
            {
              SET_ERRNO (EINVAL);
              return 0;
            }
#endif
          if (len == 7)
            {
              len += *ip++;
#if CHECK_INPUT
              if (ip >= in_end)
                {
                  SET_ERRNO (EINVAL);
                  return 0;
                }
#endif
            }

          ref -= *ip++;

          if (op + len + 2 > out_end)
            {
              SET_ERRNO (E2BIG);
              return 0;
            }
          if (ref < (u8 *)out_data)
            {
              SET_ERRNO (EINVAL);
              return 0;
            }

          len += 2;
          if (op >= ref + len)
            {
              memcpy (op, ref, len); /* disjunct areas */
              op += len;
            }
          else
            {
              do /* overlapping, copy octet by octet */
                *op++ = *ref++;
              while (--len);
            }
        }
    }

  return op - (u8 *)out_data;
}

#ifdef REDIS_TEST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include "testhelp.h"
#include "zmalloc.h"

/* Deterministic-per-seed xorshift64 PRNG. Uses a fixed-width 64-bit state so
 * the period is identical on 32- and 64-bit builds (plain "unsigned long" would
 * be 32-bit on ILP32 and this shift triple is not full-period there). */
static uint64_t lzf_rng_state;
static inline uint64_t lzf_rng (void)
{
  lzf_rng_state ^= lzf_rng_state << 13;
  lzf_rng_state ^= lzf_rng_state >> 7;
  lzf_rng_state ^= lzf_rng_state << 17;
  return lzf_rng_state;
}

/* Fill buf with data of a given "shape" to exercise every decode path:
 * incompressible (all literals), constant / low-entropy / small-period
 * (small offsets: overlap and pattern-expansion paths), and text-like
 * (mixed literals and back references). */
static void lzf_fill (u8 *buf, int len, int mode)
{
  int i;
  switch (mode)
    {
    case 0: /* incompressible */
      for (i = 0; i < len; i++) buf[i] = (u8)lzf_rng ();
      break;
    case 1: /* constant (offset 1) */
      { u8 v = (u8)lzf_rng (); for (i = 0; i < len; i++) buf[i] = v; }
      break;
    case 2: /* low entropy */
      for (i = 0; i < len; i++) buf[i] = (u8)(lzf_rng () & 3);
      break;
    case 3: /* small period => small back-reference offsets */
      { int per = 1 + (int)(lzf_rng () % 16); u8 pat[16]; int j;
        for (j = 0; j < per; j++) pat[j] = (u8)lzf_rng ();
        for (i = 0; i < len; i++) buf[i] = pat[i % per]; }
      break;
    default: /* text-like */
      { static const char *w[] = { "redis ", "the ", "quick ", "value ",
          "cache ", "field ", "data ", "store " };
        int p = 0;
        while (p < len) { const char *s = w[lzf_rng () % 8];
          while (*s && p < len) buf[p++] = (u8)*s++; } }
      break;
    }
}

/* Randomized differential fuzz test: compress random inputs and require that
 * lzf_decompress2() reproduces the original bytes exactly and agrees with the
 * untouched upstream reference decoder lzf_decompress() (see lzf_d.c).
 *
 * The seed is randomized per run (so successive CI runs explore new inputs and
 * accumulate coverage) and printed up front; pass an explicit seed as the
 * third test argument (e.g. "redis-server test lzf 123456") to reproduce a
 * specific run. */
int lzfTest (int argc, char **argv, int flags)
{
  long iterations = (flags & REDIS_TEST_ACCURATE) ? 1000000 :
                    (flags & REDIS_TEST_VALGRIND) ? 20000 : 100000;
  int max_len = 9000;
  u8 *in   = zmalloc (max_len);
  u8 *comp = zmalloc (max_len * 2 + 128);
  u8 *out1 = zmalloc (max_len);
  u8 *out2 = zmalloc (max_len);
  long tested = 0, skipped = 0, failures = 0;
  long i;

  /* Optional explicit seed as the first non-flag test argument, e.g.
   * "redis-server test lzf 123456" or "redis-server test lzf --accurate 123456".
   * argv still carries the --accurate/--valgrind/... flags here, so skip them
   * rather than mis-parsing a flag as the seed. Otherwise seed randomly (per
   * run) so CI accumulates coverage over time. */
  uint64_t seed = 0;
  int have_seed = 0;
  for (int a = 3; a < argc; a++)
    {
      if (argv[a][0] != '-')
        {
          seed = strtoull (argv[a], NULL, 0);
          have_seed = 1;
          break;
        }
    }
  if (!have_seed)
    seed = (uint64_t)time (NULL) ^ ((uint64_t)getpid () << 16);
  if (seed == 0) seed = 1; /* xorshift must not start at 0 */
  lzf_rng_state = seed;
  printf ("lzf fuzz seed=%llu (reproduce with: redis-server test lzf %llu)\n",
          (unsigned long long)seed, (unsigned long long)seed);

  /* Sizes that straddle the fast/tail boundary (LZFD_MARGIN) and the minimum
   * input threshold, mixed with random sizes. */
  static const int edge[] = { 1, 2, 16, 20, 21, 31, 32, 33, 64, 264, 271,
                              299, 300, 301, 512, 4096, 8192 };
  int n_edge = sizeof (edge) / sizeof (edge[0]);

  for (i = 0; i < iterations; i++)
    {
      int len = (i < n_edge) ? edge[i]
                             : 1 + (int)(lzf_rng () % (max_len - 1));
      int mode = (int)(lzf_rng () % 5);
      lzf_fill (in, len, mode);

      size_t clen = lzf_compress (in, len, comp, max_len * 2 + 128);
      if (clen == 0)
        {
          /* Not compressible; redis stores such values raw. Nothing to decode. */
          skipped++;
          continue;
        }

      tested++;

      memset (out1, 0x5a, len);
      memset (out2, 0xa5, len);
      size_t r1 = lzf_decompress2 (comp, clen, out1, len);
      size_t r2 = lzf_decompress  (comp, clen, out2, len);

      if (r1 != (size_t)len || r2 != (size_t)len ||
          memcmp (out1, in, len) != 0 || memcmp (out2, in, len) != 0 ||
          memcmp (out1, out2, len) != 0)
        {
          if (failures < 5)
            printf ("lzf mismatch: seed=%llu iter=%ld len=%d mode=%d clen=%zu "
                    "r1=%zu r2=%zu new_ok=%d ref_ok=%d\n",
                    (unsigned long long)seed, i, len, mode, clen, r1, r2,
                    (r1 == (size_t)len && memcmp (out1, in, len) == 0),
                    (r2 == (size_t)len && memcmp (out2, in, len) == 0));
          failures++;
        }
    }

  printf ("lzf fuzz: %ld tested, %ld skipped (incompressible), %ld failures\n",
          tested, skipped, failures);
  test_cond ("lzf_decompress2 matches the reference decoder and the original "
             "input across randomized inputs", failures == 0);

  zfree (in);
  zfree (comp);
  zfree (out1);
  zfree (out2);

  /* No test_report() here: the `test`/`test all` runner in server.c aggregates
   * __test_num/__failed_tests across all test procs and prints the summary
   * itself. Calling test_report() from a single proc would exit(1) early on an
   * unrelated earlier failure and suppress that summary. */
  return 0;
}
#endif
