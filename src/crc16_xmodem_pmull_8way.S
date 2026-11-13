#ifdef CRC16_USE_SVE
 .arch armv8.5-a+crc+crypto+sha3+sve2+sve2-aes
 .text
 .p2align 4
 .global crc16_xmodem_pmull_8way
 .type crc16_xmodem_pmull_8way,%function
crc16_xmodem_pmull_8way:
 cmp x2,#128
 b.lo crc16_xmodem
 cmp x2,#16384
 b.lo .Lmain
 cntb x3
 cmp x3,#32
 b.ne .Lmain
 ptrue p0.b
 and x3,x2,#-256
 sub x3,x3,#256
 adr x6,.LSVE_mask
 ld1b {z7.b},p0/z,[x6]
 ld1b {z20.b},p0/z,[x1,#0,mul vl]
 ld1b {z21.b},p0/z,[x1,#1,mul vl]
 ld1b {z22.b},p0/z,[x1,#2,mul vl]
 ld1b {z23.b},p0/z,[x1,#3,mul vl]
 ld1b {z24.b},p0/z,[x1,#4,mul vl]
 ld1b {z25.b},p0/z,[x1,#5,mul vl]
 ld1b {z26.b},p0/z,[x1,#6,mul vl]
 ld1b {z27.b},p0/z,[x1,#7,mul vl]
 add x1,x1,#256
 tbl z20.b,{z20.b},z7.b
 tbl z21.b,{z21.b},z7.b
 tbl z22.b,{z22.b},z7.b
 tbl z23.b,{z23.b},z7.b
 tbl z24.b,{z24.b},z7.b
 tbl z25.b,{z25.b},z7.b
 tbl z26.b,{z26.b},z7.b
 tbl z27.b,{z27.b},z7.b
 cbz w0,.LSVE_seed_ready
 lsl x0,x0,#48
 fmov d16,xzr
 ins v16.d[1],x0
 eor z20.d,z20.d,z16.d
.LSVE_seed_ready:
 cbz x3,.LSVE_post
 adr x6,.LSVE_stride
 ld1b {z17.b},p0/z,[x6]
 .p2align 4
.LSVE_loop:
 ld1b {z28.b},p0/z,[x1,#0,mul vl]
 ld1b {z29.b},p0/z,[x1,#1,mul vl]
 ld1b {z30.b},p0/z,[x1,#2,mul vl]
 ld1b {z31.b},p0/z,[x1,#3,mul vl]
 ld1b {z0.b},p0/z,[x1,#4,mul vl]
 ld1b {z1.b},p0/z,[x1,#5,mul vl]
 ld1b {z2.b},p0/z,[x1,#6,mul vl]
 ld1b {z3.b},p0/z,[x1,#7,mul vl]
 add x1,x1,#256
 prfm pldl2strm,[x1,#1024]
 tbl z28.b,{z28.b},z7.b
 tbl z29.b,{z29.b},z7.b
 tbl z30.b,{z30.b},z7.b
 tbl z31.b,{z31.b},z7.b
 tbl z0.b,{z0.b},z7.b
 tbl z1.b,{z1.b},z7.b
 tbl z2.b,{z2.b},z7.b
 tbl z3.b,{z3.b},z7.b
 pmullt z4.q,z20.d,z17.d
 pmullt z5.q,z21.d,z17.d
 pmullt z6.q,z22.d,z17.d
 pmullt z18.q,z23.d,z17.d
 pmullb z20.q,z20.d,z17.d
 pmullb z21.q,z21.d,z17.d
 pmullb z22.q,z22.d,z17.d
 pmullb z23.q,z23.d,z17.d
 eor3 z20.d,z20.d,z4.d,z28.d
 eor3 z21.d,z21.d,z5.d,z29.d
 eor3 z22.d,z22.d,z6.d,z30.d
 eor3 z23.d,z23.d,z18.d,z31.d
 pmullt z4.q,z24.d,z17.d
 pmullt z5.q,z25.d,z17.d
 pmullt z6.q,z26.d,z17.d
 pmullt z18.q,z27.d,z17.d
 pmullb z24.q,z24.d,z17.d
 pmullb z25.q,z25.d,z17.d
 pmullb z26.q,z26.d,z17.d
 pmullb z27.q,z27.d,z17.d
 eor3 z24.d,z24.d,z4.d,z0.d
 eor3 z25.d,z25.d,z5.d,z1.d
 eor3 z26.d,z26.d,z6.d,z2.d
 eor3 z27.d,z27.d,z18.d,z3.d
 subs x3,x3,#256
 b.ne .LSVE_loop
.LSVE_post:
 and x2,x2,#255
 adr x6,.LSVE_merge
 ld1b {z16.b},p0/z,[x6]
 pmullt z0.q,z20.d,z16.d
 pmullt z1.q,z21.d,z16.d
 pmullt z2.q,z22.d,z16.d
 pmullt z3.q,z23.d,z16.d
 pmullb z20.q,z20.d,z16.d
 pmullb z21.q,z21.d,z16.d
 pmullb z22.q,z22.d,z16.d
 pmullb z23.q,z23.d,z16.d
 eor3 z24.d,z24.d,z20.d,z0.d
 eor3 z25.d,z25.d,z21.d,z1.d
 eor3 z26.d,z26.d,z22.d,z2.d
 eor3 z27.d,z27.d,z23.d,z3.d
 mov v0.16b,v24.16b
 ext z24.b,z24.b,z24.b,#16
 mov v1.16b,v24.16b
 mov v2.16b,v25.16b
 ext z25.b,z25.b,z25.b,#16
 mov v3.16b,v25.16b
 mov v4.16b,v26.16b
 ext z26.b,z26.b,z26.b,#16
 mov v5.16b,v26.16b
 mov v6.16b,v27.16b
 ext z27.b,z27.b,z27.b,#16
 mov v7.16b,v27.16b
 adr x6,.Ltree
 ldp q16,q17,[x6]
 ldr q18,[x6,#32]
 pmull2 v20.1q, v0.2d, v18.2d
 pmull2 v21.1q, v1.2d, v18.2d
 pmull2 v22.1q, v2.2d, v18.2d
 pmull2 v23.1q, v3.2d, v18.2d
 pmull v0.1q, v0.1d, v18.1d
 pmull v1.1q, v1.1d, v18.1d
 pmull v2.1q, v2.1d, v18.1d
 pmull v3.1q, v3.1d, v18.1d
#ifdef CRC16_USE_SHA3
 eor3 v4.16b, v0.16b, v20.16b, v4.16b
#else
 eor v4.16b, v4.16b, v0.16b
 eor v4.16b, v4.16b, v20.16b
#endif
#ifdef CRC16_USE_SHA3
 eor3 v5.16b, v1.16b, v21.16b, v5.16b
#else
 eor v5.16b, v5.16b, v1.16b
 eor v5.16b, v5.16b, v21.16b
#endif
#ifdef CRC16_USE_SHA3
 eor3 v6.16b, v2.16b, v22.16b, v6.16b
#else
 eor v6.16b, v6.16b, v2.16b
 eor v6.16b, v6.16b, v22.16b
#endif
#ifdef CRC16_USE_SHA3
 eor3 v7.16b, v3.16b, v23.16b, v7.16b
#else
 eor v7.16b, v7.16b, v3.16b
 eor v7.16b, v7.16b, v23.16b
#endif
 pmull2 v20.1q, v4.2d, v17.2d
 pmull2 v21.1q, v5.2d, v17.2d
 pmull v4.1q, v4.1d, v17.1d
 pmull v5.1q, v5.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v6.16b, v4.16b, v20.16b, v6.16b
#else
 eor v6.16b, v6.16b, v4.16b
 eor v6.16b, v6.16b, v20.16b
#endif
#ifdef CRC16_USE_SHA3
 eor3 v7.16b, v5.16b, v21.16b, v7.16b
#else
 eor v7.16b, v7.16b, v5.16b
 eor v7.16b, v7.16b, v21.16b
#endif
 pmull2 v20.1q, v6.2d, v16.2d
 pmull v6.1q, v6.1d, v16.1d
#ifdef CRC16_USE_SHA3
 eor3 v7.16b, v6.16b, v20.16b, v7.16b
#else
 eor v7.16b, v7.16b, v6.16b
 eor v7.16b, v7.16b, v20.16b
#endif
 mov v2.16b,v7.16b
 b .Lreduce
 .size crc16_xmodem_pmull_8way,.-crc16_xmodem_pmull_8way
 .p2align 5
.LSVE_mask:
 .byte 15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16
.LSVE_stride:
 .quad 0x16270000,0xcacd0000
 .quad 0x16270000,0xcacd0000
.LSVE_merge:
 .quad 0xb5b70000,0x18d30000,0xb5b70000,0x18d30000
#endif
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
#ifndef CRC16_USE_SVE
 .global crc16_xmodem_pmull_8way
 .set crc16_xmodem_pmull_8way,crc16_xmodem_pmull_8way_neon
#endif
 .global crc16_xmodem_pmull_8way_neon
 .type crc16_xmodem_pmull_8way_neon, %function
crc16_xmodem_pmull_8way_neon:
 cmp x2, #128
 b.lo crc16_xmodem
.Lmain:
 and x3, x2, #-128
 sub x3, x3, #128
 ldr q7, .Lmask
 ld1 {v20.16b-v23.16b}, [x1], #64
 ld1 {v24.16b-v27.16b}, [x1], #64
 tbl v20.16b, {v20.16b}, v7.16b
 tbl v21.16b, {v21.16b}, v7.16b
 tbl v22.16b, {v22.16b}, v7.16b
 tbl v23.16b, {v23.16b}, v7.16b
 tbl v24.16b, {v24.16b}, v7.16b
 tbl v25.16b, {v25.16b}, v7.16b
 tbl v26.16b, {v26.16b}, v7.16b
 tbl v27.16b, {v27.16b}, v7.16b
 cbz w0, .Lmain_seed_ready
 lsl x0, x0, #48
 fmov d16, xzr
 ins v16.d[1], x0
 eor v20.16b, v20.16b, v16.16b
.Lmain_seed_ready:
 cbz x3, .Lmain_post
 ldr q17, .Lstride8
 .p2align 4
.Lmain_loop:
 ld1 {v28.16b-v31.16b}, [x1], #64
 ld1 {v0.16b-v3.16b}, [x1], #64
 prfm pldl2strm, [x1,#1024]
 tbl v28.16b, {v28.16b}, v7.16b
 tbl v29.16b, {v29.16b}, v7.16b
 tbl v30.16b, {v30.16b}, v7.16b
 tbl v31.16b, {v31.16b}, v7.16b
 tbl v0.16b, {v0.16b}, v7.16b
 tbl v1.16b, {v1.16b}, v7.16b
 tbl v2.16b, {v2.16b}, v7.16b
 tbl v3.16b, {v3.16b}, v7.16b
 pmull2 v4.1q, v20.2d, v17.2d
 pmull v20.1q, v20.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v20.16b, v20.16b, v4.16b, v28.16b
#else
 eor v20.16b, v20.16b, v4.16b
 eor v20.16b, v20.16b, v28.16b
#endif
 pmull2 v4.1q, v21.2d, v17.2d
 pmull v21.1q, v21.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v21.16b, v21.16b, v4.16b, v29.16b
#else
 eor v21.16b, v21.16b, v4.16b
 eor v21.16b, v21.16b, v29.16b
#endif
 pmull2 v4.1q, v22.2d, v17.2d
 pmull v22.1q, v22.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v22.16b, v22.16b, v4.16b, v30.16b
#else
 eor v22.16b, v22.16b, v4.16b
 eor v22.16b, v22.16b, v30.16b
#endif
 pmull2 v4.1q, v23.2d, v17.2d
 pmull v23.1q, v23.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v23.16b, v23.16b, v4.16b, v31.16b
#else
 eor v23.16b, v23.16b, v4.16b
 eor v23.16b, v23.16b, v31.16b
#endif
 pmull2 v4.1q, v24.2d, v17.2d
 pmull v24.1q, v24.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v24.16b, v24.16b, v4.16b, v0.16b
#else
 eor v24.16b, v24.16b, v4.16b
 eor v24.16b, v24.16b, v0.16b
#endif
 pmull2 v4.1q, v25.2d, v17.2d
 pmull v25.1q, v25.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v25.16b, v25.16b, v4.16b, v1.16b
#else
 eor v25.16b, v25.16b, v4.16b
 eor v25.16b, v25.16b, v1.16b
#endif
 pmull2 v4.1q, v26.2d, v17.2d
 pmull v26.1q, v26.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v26.16b, v26.16b, v4.16b, v2.16b
#else
 eor v26.16b, v26.16b, v4.16b
 eor v26.16b, v26.16b, v2.16b
#endif
 pmull2 v4.1q, v27.2d, v17.2d
 pmull v27.1q, v27.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v27.16b, v27.16b, v4.16b, v3.16b
#else
 eor v27.16b, v27.16b, v4.16b
 eor v27.16b, v27.16b, v3.16b
#endif
 subs x3, x3, #128
 b.ne .Lmain_loop
.Lmain_post:
 and x2, x2, #127
 adr x6, .Ltree
 ldp q16, q17, [x6]
 ldr q18, [x6,#32]
 pmull2 v4.1q, v20.2d, v18.2d
 pmull2 v5.1q, v21.2d, v18.2d
 pmull2 v6.1q, v22.2d, v18.2d
 pmull2 v19.1q, v23.2d, v18.2d
 pmull v20.1q, v20.1d, v18.1d
 pmull v21.1q, v21.1d, v18.1d
 pmull v22.1q, v22.1d, v18.1d
 pmull v23.1q, v23.1d, v18.1d
#ifdef CRC16_USE_SHA3
 eor3 v24.16b, v20.16b, v4.16b, v24.16b
#else
 eor v24.16b, v24.16b, v20.16b
 eor v24.16b, v24.16b, v4.16b
#endif
#ifdef CRC16_USE_SHA3
 eor3 v25.16b, v21.16b, v5.16b, v25.16b
#else
 eor v25.16b, v25.16b, v21.16b
 eor v25.16b, v25.16b, v5.16b
#endif
#ifdef CRC16_USE_SHA3
 eor3 v26.16b, v22.16b, v6.16b, v26.16b
#else
 eor v26.16b, v26.16b, v22.16b
 eor v26.16b, v26.16b, v6.16b
#endif
#ifdef CRC16_USE_SHA3
 eor3 v27.16b, v23.16b, v19.16b, v27.16b
#else
 eor v27.16b, v27.16b, v23.16b
 eor v27.16b, v27.16b, v19.16b
#endif
 pmull2 v4.1q, v24.2d, v17.2d
 pmull2 v5.1q, v25.2d, v17.2d
 pmull v24.1q, v24.1d, v17.1d
 pmull v25.1q, v25.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v26.16b, v24.16b, v4.16b, v26.16b
#else
 eor v26.16b, v26.16b, v24.16b
 eor v26.16b, v26.16b, v4.16b
#endif
#ifdef CRC16_USE_SHA3
 eor3 v27.16b, v25.16b, v5.16b, v27.16b
#else
 eor v27.16b, v27.16b, v25.16b
 eor v27.16b, v27.16b, v5.16b
#endif
 pmull2 v4.1q, v26.2d, v16.2d
 pmull v26.1q, v26.1d, v16.1d
#ifdef CRC16_USE_SHA3
 eor3 v27.16b, v26.16b, v4.16b, v27.16b
#else
 eor v27.16b, v27.16b, v26.16b
 eor v27.16b, v27.16b, v4.16b
#endif
 mov v2.16b, v27.16b
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
 .size crc16_xmodem_pmull_8way_neon, .-crc16_xmodem_pmull_8way_neon
 .p2align 4
.Lmask:
 .byte 15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0
 .p2align 4
.Ltree:
 .quad 0x45630000,0xd5f60000
 .quad 0xf0e60000,0xf44b0000
 .quad 0x59b00000,0x60190000
 .quad 0xb5b70000,0x18d30000
.Lstride8:
 .quad 0xb5b70000,0x18d30000
.Lreciprocal:
 .quad 0x11303471a041b343,0x11303471a041b343
 .quad 0xe569d503417ca4a2,0xe569d503417ca4a2
 .section .note.GNU-stack,"",%progbits
