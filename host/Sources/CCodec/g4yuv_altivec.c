/*
 * g4yuv_altivec.c — 4:2:2 video to A,R,G,B, 8 pixels at a time with AltiVec
 * (G4 and G5). Used by g4codec.c when built with -DG4_HAVE_ALTIVEC; on any
 * other build this file compiles to nothing.
 *
 * Same integer maths as g4_yuv_to_pair, and AltiVec's saturating pack is
 * the same clamp, so the output is bit-identical (test/test_yuv_simd.c).
 */
#include <stdint.h>
#include <string.h>

#if defined(__ALTIVEC__)
#include <altivec.h>
#undef vector
#undef pixel
#undef bool
#define G4V __vector
static const unsigned char yuv_permY[16]  __attribute__((aligned(16))) = { 0,17, 0,19, 0,21, 0,23, 0,25, 0,27, 0,29, 0,31 };
static const unsigned char yuv_permCb[16] __attribute__((aligned(16))) = { 0,16, 0,16, 0,20, 0,20, 0,24, 0,24, 0,28, 0,28 };
static const unsigned char yuv_permCr[16] __attribute__((aligned(16))) = { 0,18, 0,18, 0,22, 0,22, 0,26, 0,26, 0,30, 0,30 };
static const short yuv_k[5][8] __attribute__((aligned(16))) = {
    { 128, 128, 128, 128, 128, 128, 128, 128 },
    { 359, 359, 359, 359, 359, 359, 359, 359 },
    {  88,  88,  88,  88,  88,  88,  88,  88 },
    { 183, 183, 183, 183, 183, 183, 183, 183 },
    { 454, 454, 454, 454, 454, 454, 454, 454 } };
/* per-channel bias added before >>8, and offset subtracted after */
static const int yuv_b[6][4] __attribute__((aligned(16))) = {
    { 46208, 46208, 46208, 46208 }, { 180, 180, 180, 180 },     /* R */
    { 34944, 34944, 34944, 34944 }, { 136, 136, 136, 136 },     /* G */
    { 58240, 58240, 58240, 58240 }, { 227, 227, 227, 227 } };   /* B */

void g4_yuv8_altivec(const uint8_t *in, uint8_t *out)
{
    G4V unsigned char zero = vec_splat_u8(0);
    G4V unsigned char v = vec_perm(vec_ld(0, in), vec_ld(15, in), vec_lvsl(0, in));   /* unaligned load */
    G4V signed short y16  = (G4V signed short)vec_perm(zero, v, vec_ld(0, yuv_permY));
    G4V signed short k128 = vec_ld(0, yuv_k[0]);
    G4V signed short c16  = vec_sub((G4V signed short)vec_perm(zero, v, vec_ld(0, yuv_permCb)), k128);
    G4V signed short d16  = vec_sub((G4V signed short)vec_perm(zero, v, vec_ld(0, yuv_permCr)), k128);
    G4V unsigned int eight = vec_splat_u32(8);
    G4V signed int rd, gd, bd;
    G4V signed short rs, gs, bs;
    G4V unsigned char r8, g8, b8, a8 = vec_splat_u8(-1), ar, gb;
    G4V unsigned char lo, hi;

    /* even lanes of c16/d16 hold the 4 pairs' chroma: 32-bit products */
    rd = vec_sub(vec_sra(vec_add(vec_mule(d16, vec_ld(0, yuv_k[1])), vec_ld(0, yuv_b[0])), eight), vec_ld(0, yuv_b[1]));
    gd = vec_sub(vec_sra(vec_add(vec_add(vec_mule(c16, vec_ld(0, yuv_k[2])), vec_mule(d16, vec_ld(0, yuv_k[3]))),
                                 vec_ld(0, yuv_b[2])), eight), vec_ld(0, yuv_b[3]));
    bd = vec_sub(vec_sra(vec_add(vec_mule(c16, vec_ld(0, yuv_k[4])), vec_ld(0, yuv_b[4])), eight), vec_ld(0, yuv_b[5]));

    /* one value per pair -> one per pixel */
    rs = vec_pack(rd, rd); rs = vec_mergeh(rs, rs);
    gs = vec_pack(gd, gd); gs = vec_mergeh(gs, gs);
    bs = vec_pack(bd, bd); bs = vec_mergeh(bs, bs);

    r8 = vec_packsu(vec_add(y16, rs), vec_add(y16, rs));      /* saturating = clamp to 0..255 */
    g8 = vec_packsu(vec_sub(y16, gs), vec_sub(y16, gs));
    b8 = vec_packsu(vec_add(y16, bs), vec_add(y16, bs));

    ar = vec_mergeh(a8, r8);                                     /* A R A R ... */
    gb = vec_mergeh(g8, b8);                                     /* G B G B ... */
    lo = (G4V unsigned char)vec_mergeh((G4V unsigned short)ar, (G4V unsigned short)gb);
    hi = (G4V unsigned char)vec_mergel((G4V unsigned short)ar, (G4V unsigned short)gb);
    if (((unsigned long)out & 15) == 0) {
        vec_st(lo, 0, out); vec_st(hi, 16, out);
    } else {
        unsigned char tmp[32] __attribute__((aligned(16)));
        vec_st(lo, 0, tmp); vec_st(hi, 16, tmp);
        memcpy(out, tmp, 32);
    }
}
#else
typedef int g4yuv_altivec_unused;   /* nothing to build without AltiVec */
#endif

