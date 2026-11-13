/* General CRC16/XMODEM folding kernel. Arbitrary seed and byte length.
 * Caller-saved SIMD registers only. Safe exact in-buffer loads.
 * Reciprocal reduction and balanced polynomial merges. */
#ifdef CRC16_USE_SHA3
 .arch armv8.2-a+crc+crypto+sha3
#else
 .arch armv8-a+crc+crypto
#endif
 .text
 .p2align 4
 .global crc16_xmodem_pmull
 .type crc16_xmodem_pmull, %function
crc16_xmodem_pmull:
 cmp x2, #128
 b.lo crc16_xmodem
.Lmain:
 and x3, x2, #-64
 sub x3, x3, #64
 ldr q7, .Lmask
 ld1 {v2.16b-v5.16b}, [x1], #64
 tbl v2.16b, {v2.16b}, v7.16b
 tbl v3.16b, {v3.16b}, v7.16b
 tbl v4.16b, {v4.16b}, v7.16b
 tbl v5.16b, {v5.16b}, v7.16b
 cbz w0, .Lmain_seed_ready
 lsl x0, x0, #48
 fmov d16, xzr
 ins v16.d[1], x0
 eor v2.16b, v2.16b, v16.16b
.Lmain_seed_ready:
 cbz x3, .Lmain_post
 ldr q17, .Lstride4
 .p2align 4
.Lmain_loop:
 ld1 {v28.16b-v31.16b}, [x1], #64
 prfm pldl2strm, [x1,#1024]
 tbl v28.16b, {v28.16b}, v7.16b
 tbl v29.16b, {v29.16b}, v7.16b
 tbl v30.16b, {v30.16b}, v7.16b
 tbl v31.16b, {v31.16b}, v7.16b
 pmull2 v24.1q, v2.2d, v17.2d
 pmull v2.1q, v2.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v2.16b, v2.16b, v24.16b, v28.16b
#else
 eor v2.16b, v2.16b, v24.16b
 eor v2.16b, v2.16b, v28.16b
#endif
 pmull2 v24.1q, v3.2d, v17.2d
 pmull v3.1q, v3.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v3.16b, v3.16b, v24.16b, v29.16b
#else
 eor v3.16b, v3.16b, v24.16b
 eor v3.16b, v3.16b, v29.16b
#endif
 pmull2 v24.1q, v4.2d, v17.2d
 pmull v4.1q, v4.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v4.16b, v4.16b, v24.16b, v30.16b
#else
 eor v4.16b, v4.16b, v24.16b
 eor v4.16b, v4.16b, v30.16b
#endif
 pmull2 v24.1q, v5.2d, v17.2d
 pmull v5.1q, v5.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v5.16b, v5.16b, v24.16b, v31.16b
#else
 eor v5.16b, v5.16b, v24.16b
 eor v5.16b, v5.16b, v31.16b
#endif
 subs x3, x3, #64
 b.ne .Lmain_loop
.Lmain_post:
 and x2, x2, #63
 adr x6, .Ltree
 ldp q16, q17, [x6]
 pmull2 v6.1q, v2.2d, v17.2d
 pmull2 v18.1q, v3.2d, v17.2d
 pmull v2.1q, v2.1d, v17.1d
 pmull v3.1q, v3.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v4.16b, v2.16b, v6.16b, v4.16b
#else
 eor v4.16b, v4.16b, v2.16b
 eor v4.16b, v4.16b, v6.16b
#endif
#ifdef CRC16_USE_SHA3
 eor3 v5.16b, v3.16b, v18.16b, v5.16b
#else
 eor v5.16b, v5.16b, v3.16b
 eor v5.16b, v5.16b, v18.16b
#endif
 pmull2 v6.1q, v4.2d, v16.2d
 pmull v4.1q, v4.1d, v16.1d
#ifdef CRC16_USE_SHA3
 eor3 v5.16b, v4.16b, v6.16b, v5.16b
#else
 eor v5.16b, v5.16b, v4.16b
 eor v5.16b, v5.16b, v6.16b
#endif
 mov v2.16b, v5.16b
 b .Lreduce
.Lreduce:
 adr x6, .Lreciprocal
 ldp q16, q17, [x6]
 pmull2 v0.1q, v2.2d, v16.2d
 pmull v1.1q, v2.1d, v16.1d
 pmull2 v3.1q, v2.2d, v17.2d
 eor v1.16b, v1.16b, v3.16b
 ext v1.16b, v1.16b, v1.16b, #8
#ifdef CRC16_USE_SHA3
 eor3 v1.16b, v1.16b, v0.16b, v2.16b
#else
 eor v1.16b, v1.16b, v0.16b
 eor v1.16b, v1.16b, v2.16b
#endif
 umov w4, v1.h[0]
 eor w0, w4, w4, lsl #5
 eor w0, w0, w4, lsl #12
 uxth w0, w0
 cbnz x2, crc16_xmodem
 ret
 .size crc16_xmodem_pmull, .-crc16_xmodem_pmull
 .p2align 4
.Lmask:
 .byte 15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0
 .p2align 4
.Ltree:
 .quad 0x45630000,0xd5f60000
 .quad 0xf0e60000,0xf44b0000
 .quad 0x59b00000,0x60190000
 .quad 0xb5b70000,0x18d30000
.Lstride4:
 .quad 0x59b00000,0x60190000
.Lreciprocal:
 .quad 0x11303471a041b343,0x11303471a041b343
 .quad 0xe569d503417ca4a2,0xe569d503417ca4a2
 .section .note.GNU-stack,"",%progbits
