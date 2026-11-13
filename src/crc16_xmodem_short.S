/////////////////////////////////////////////////////////////////////////
// XMODEM CRC16 — Short buffer NEON/PMULL path with scalable long-input delegation
//
// Folds whole 16-byte blocks with the x^128 constant, then handles a trailing
// partial block of r bytes by folding the state by x^(8r) and adding the r bytes
// (big-endian, at the bottom of the block).  This is the correct continuation;
// zero-padding the block and folding by x^128 is not.
//
// Precondition: len >= 16 (the dispatcher routes len < 16 to the table path).
//
// uint16_t crc16_xmodem_short(uint16_t seed, const uint8_t *buf, uint64_t len);
/////////////////////////////////////////////////////////////////////////

#ifdef CRC16_USE_SHA3
 .arch armv8.2-a+crc+crypto+sha3
#else
	.arch armv8-a+crc+crypto
#endif
	.text
 .p2align 6
	.align	3
	.global	crc16_xmodem_short
#ifndef __APPLE__
	.type	crc16_xmodem_short, %function
#endif

crc16_xmodem_short:
	/* Scale seed to 32-bit domain: seed << 48 in the high d-lane.
	   lsl #48 discards bits 16..63, so no explicit uxth is needed. */
	lsl	x0, x0, 48
	fmov	d16, xzr
	ins	v16.d[1], x0

	/* Load byte-reverse shuffle mask (PC-relative) */
	ldr	q7, shuffle_mask_short

	/* First 16B block: byte-reverse and XOR in the seed */
	ld1	{v28.16b}, [x1], #16
	sub	x2, x2, #16
	tbl	v28.16b, {v28.16b}, v7.16b
	eor	v2.16b, v16.16b, v28.16b
	cbz	x2, .crc_short_reduce
 cmp x2, #112
 b.hs .overall_short_long

	ldr	q17, fold_constant_p1

	/* Remaining 16B blocks: fold state by x^128, XOR in new data */
	.align 2
.crc_short_loop:
	cmp	x2, #16
	b.lo	.crc_short_partial
	ld1	{v28.16b}, [x1], #16
	sub	x2, x2, #16
	tbl	v28.16b, {v28.16b}, v7.16b
	pmull2	v24.1q, v2.2d, v17.2d
	pmull	v2.1q,  v2.1d, v17.1d
	eor	v2.16b, v24.16b, v2.16b
	eor	v2.16b, v2.16b,  v28.16b
	cbnz	x2, .crc_short_loop
	b	.crc_short_reduce

	/* Trailing 1..15 bytes: fold state by x^(8r) and add T_bot (the r bytes
	   big-endian at the block bottom).  The last 16 bytes are loaded
	   directly (safe: the dispatcher only enters with len >= 16) and a
	   single TBL selects/positions them, masking the already-folded prefix
	   bytes above. */
.crc_short_partial:
	add	x4, x1, x2		/* x4 = buffer end */
	ldr	q28, [x4, #-16]
	movi	v29.16b, 0
	adr	x6, crc_short_tail_idx
	ldr	q16, [x6, x2, lsl #4]
	tbl	v28.16b, {v28.16b, v29.16b}, v16.16b	/* -> T_bot */

	adr	x6, crc_short_partial_consts
	ldr	q17, [x6, x2, lsl #4]
	pmull2	v24.1q, v2.2d, v17.2d
	pmull	v2.1q,  v2.1d, v17.1d
	eor	v2.16b, v24.16b, v2.16b
	eor	v2.16b, v2.16b,  v28.16b

/* ================================================================= */
/* P0 fold + Barrett reduction                                       */
/* ================================================================= */
.crc_short_reduce:

 /* K=floor(x^144/P)=x^128+KH*x^64+KL.
  * For A=H*x^64+L, low16(floor(A*x^16/P)) is
  * low16(L + H*KH + ((L*KH + H*KL)>>64)).
  * The CRC is low16(q*(1+x^5+x^12)). Products are carry-less.
  */
 adr x6, reciprocal128_constants
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
 ret

#ifndef __APPLE__
	.size	crc16_xmodem_short, .-crc16_xmodem_short
#endif

/* ================================================================= */
/* Constants                                                         */
/* ================================================================= */
	.align	4
fold_constant_p1:
	.word	0x45630000	/* P1_lo = x^128 mod P_scaled */
	.word	0x00000000
	.word	0xD5F60000	/* P1_hi = x^192 mod P_scaled */
	.word	0x00000000

	.align	4
shuffle_mask_short:
	.byte	15, 14, 13, 12, 11, 10, 9, 8
	.byte	7,   6,  5,  4,  3,  2, 1, 0
	.align	4
.shuffle_mask_lanchor_short = shuffle_mask_short

	/* TBL index vectors for the trailing partial block, indexed by r (1..15):
	   byte k <- 15-k for k<r (reversed residual), else 16 (zero register). */
	.align	4
crc_short_tail_idx:
	.byte	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
	.byte	15, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
	.byte	15, 14, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
	.byte	15, 14, 13, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
	.byte	15, 14, 13, 12, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
	.byte	15, 14, 13, 12, 11, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
	.byte	15, 14, 13, 12, 11, 10, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
	.byte	15, 14, 13, 12, 11, 10,  9, 16, 16, 16, 16, 16, 16, 16, 16, 16
	.byte	15, 14, 13, 12, 11, 10,  9,  8, 16, 16, 16, 16, 16, 16, 16, 16
	.byte	15, 14, 13, 12, 11, 10,  9,  8,  7, 16, 16, 16, 16, 16, 16, 16
	.byte	15, 14, 13, 12, 11, 10,  9,  8,  7,  6, 16, 16, 16, 16, 16, 16
	.byte	15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5, 16, 16, 16, 16, 16
	.byte	15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4, 16, 16, 16, 16
	.byte	15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3, 16, 16, 16
	.byte	15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2, 16, 16
	.byte	15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1, 16

	/* Partial-block fold constants, indexed by r = len % 16 (1..15):
	   entry r = { x^(8r) mod P_scaled, x^(8r+64) mod P_scaled }. */
	.align	4
crc_short_partial_consts:
	.quad	0x0000000000000000	/* r=0 (unused) */
	.quad	0x0000000000000000
	.quad	0x0000000000000100	/* r=1  */
	.quad	0x0000000045A00000
	.quad	0x0000000000010000	/* r=2  */
	.quad	0x00000000B8610000
	.quad	0x0000000001000000	/* r=3  */
	.quad	0x0000000047D30000
	.quad	0x0000000010210000	/* r=4  */
	.quad	0x00000000EB230000
	.quad	0x0000000033310000	/* r=5  */
	.quad	0x000000006F450000
	.quad	0x0000000037300000	/* r=6  */
	.quad	0x00000000D8490000
	.quad	0x0000000076B40000	/* r=7  */
	.quad	0x0000000003750000
	.quad	0x00000000AA510000	/* r=8  */
	.quad	0x0000000045630000
	.quad	0x0000000045A00000	/* r=9  */
	.quad	0x000000007B610000
	.quad	0x00000000B8610000	/* r=10 */
	.quad	0x00000000AEFC0000
	.quad	0x0000000047D30000	/* r=11 */
	.quad	0x00000000A8240000
	.quad	0x00000000EB230000	/* r=12 */
	.quad	0x0000000010E20000
	.quad	0x000000006F450000	/* r=13 */
	.quad	0x00000000F0310000
	.quad	0x00000000D8490000	/* r=14 */
	.quad	0x00000000DE1F0000
	.quad	0x0000000003750000	/* r=15 */
	.quad	0x0000000035B30000


/* Public dispatcher: keep exact 16B input on a short inline entry path.
 * No seed/data assumptions; other lengths retain the generic dispatcher.
 */
 .align 4
 .global crc16_xmodem
 .type crc16_xmodem, %function
 .p2align 4
crc16_xmodem:
 cmp x2, #16
 b.ne .dispatch_other_length
 cbnz w0, crc16_xmodem_short
 ldr q7, shuffle_mask_short
 ldr q2, [x1]
 tbl v2.16b, {v2.16b}, v7.16b

 /* K=floor(x^144/P)=x^128+KH*x^64+KL.
  * For A=H*x^64+L, low16(floor(A*x^16/P)) is
  * low16(L + H*KH + ((L*KH + H*KL)>>64)).
  * The CRC is low16(q*(1+x^5+x^12)). Products are carry-less.
  */
 adr x6, reciprocal128_constants
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
 ret
.dispatch_other_length:
 cmp x2, #64
 b.ne .overall_tiny_router
 ldr q7, shuffle_mask_short
 ld1 {v0.16b-v3.16b}, [x1]
 tbl v0.16b, {v0.16b}, v7.16b
 tbl v1.16b, {v1.16b}, v7.16b
 tbl v2.16b, {v2.16b}, v7.16b
 tbl v3.16b, {v3.16b}, v7.16b
 cbz w0, .inline_all64_seed_ready
 lsl x0, x0, #48
 fmov d31, xzr
 ins v31.d[1], x0
 eor v0.16b, v0.16b, v31.16b
.inline_all64_seed_ready:
 adr x6, deep_fold_constants
 ldp q16, q17, [x6]
 pmull2 v4.1q, v0.2d, v17.2d
 pmull v0.1q, v0.1d, v17.1d
 pmull2 v5.1q, v1.2d, v17.2d
 pmull v1.1q, v1.1d, v17.1d
#ifdef CRC16_USE_SHA3
 eor3 v0.16b, v0.16b, v4.16b, v2.16b
#else
 eor v0.16b, v0.16b, v4.16b
 eor v0.16b, v0.16b, v2.16b
#endif
#ifdef CRC16_USE_SHA3
 eor3 v1.16b, v1.16b, v5.16b, v3.16b
#else
 eor v1.16b, v1.16b, v5.16b
 eor v1.16b, v1.16b, v3.16b
#endif

 pmull2 v6.1q, v0.2d, v16.2d
 pmull v0.1q, v0.1d, v16.1d
#ifdef CRC16_USE_SHA3
 eor3 v2.16b, v0.16b, v6.16b, v1.16b
#else
 eor v2.16b, v0.16b, v6.16b
 eor v2.16b, v2.16b, v1.16b
#endif


 /* K=floor(x^144/P)=x^128+KH*x^64+KL.
  * For A=H*x^64+L, low16(floor(A*x^16/P)) is
  * low16(L + H*KH + ((L*KH + H*KL)>>64)).
  * The CRC is low16(q*(1+x^5+x^12)). Products are carry-less.
  */
 adr x6, reciprocal128_constants
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
 ret

 .size crc16_xmodem, .-crc16_xmodem



.overall_short_long:
 lsr x0, x0, #48
 sub x1, x1, #16
 add x2, x2, #16
 b crc16_xmodem_generic
.overall_tiny_router:
 /* 8..15 bytes, seed=0: two overlapping 8-byte loads stay in bounds.
  * Construct the exact big-endian polynomial in v2, without zero-padding
  * the logical message. Other lengths/seeds retain the generic fallback. */
 sub x3, x2, #8
 cmp x3, #7
 b.hi crc16_xmodem_generic
 cbnz w0, crc16_xmodem_generic
 ldr x3, [x1]
 add x4, x1, x2
 ldr x5, [x4,#-8]
 rev x3, x3
 rev x5, x5
 mov x4, #16
 sub x4, x4, x2
 lsl x4, x4, #3
 lsr x3, x3, x4
 cmp x2, #8
 csel x3, xzr, x3, eq
 fmov d2, x5
 ins v2.d[1], x3
 b .crc_short_reduce
 .p2align 4
reciprocal128_constants:
 .quad 0x11303471a041b343,0x11303471a041b343
 .quad 0xe569d503417ca4a2,0xe569d503417ca4a2
 .quad 0x1021

 .p2align 6
deep_fold_constants:
 .quad 0x45630000,0xd5f60000,0xf0e60000,0xf44b0000

 .section .note.GNU-stack,"",%progbits
