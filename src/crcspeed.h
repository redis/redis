/* Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE. */

#ifndef CRCSPEED_H
#define CRCSPEED_H

#include <inttypes.h>
#include <stdio.h>

typedef uint64_t (*crcfn64)(uint64_t, const void *, const uint64_t);
typedef uint16_t (*crcfn16)(uint16_t, const void *, const uint64_t);

void set_crc64_cutoffs(size_t dual_cutoff, size_t tri_cutoff);

/* CRC-64 */
void crcspeed64little_init(crcfn64 fn, uint64_t table[8][256]);
void crcspeed64big_init(crcfn64 fn, uint64_t table[8][256]);
void crcspeed64native_init(crcfn64 fn, uint64_t table[8][256]);

uint64_t crcspeed64little(uint64_t table[8][256], uint64_t crc, void *buf,
                          size_t len);
uint64_t crcspeed64big(uint64_t table[8][256], uint64_t crc, void *buf,
                       size_t len);
uint64_t crcspeed64native(uint64_t table[8][256], uint64_t crc, void *buf,
                          size_t len);

/* CRC-16 */
void crcspeed16little_init(crcfn16 fn, uint16_t table[8][256]);
void crcspeed16big_init(crcfn16 fn, uint16_t table[8][256]);
void crcspeed16native_init(crcfn16 fn, uint16_t table[8][256]);

uint16_t crcspeed16little(uint16_t table[8][256], uint16_t crc, void *buf,
                          size_t len);
uint16_t crcspeed16big(uint16_t table[8][256], uint16_t crc, void *buf,
                       size_t len);
uint16_t crcspeed16native(uint16_t table[8][256], uint16_t crc, void *buf,
                          size_t len);
#endif
