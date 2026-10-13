/* g4_yuv_row (AltiVec on G4/G5) must equal g4_yuv_to_pair bit for bit:
   exhaustive over Cb x Cr x Y0 (Y1 varied), plus random rows of every width
   at unaligned addresses. */
#include "g4codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    static uint8_t in[64 + 16], out[128 + 16], ref[128 + 16];
    unsigned cb, cr, y, i, w, off, bad = 0;
    uint32_t r = 1;
#ifdef __ALTIVEC__
    puts("AltiVec path enabled");
#else
    puts("scalar path (no AltiVec in this build)");
#endif
    for (cb = 0; cb < 256; cb++)
        for (cr = 0; cr < 256; cr++)
            for (y = 0; y < 256; y += 4) {             /* 4 pairs per call: Y0 = y..y+3 */
                for (i = 0; i < 4; i++) {
                    in[4 * i] = (uint8_t)cb; in[4 * i + 1] = (uint8_t)(y + i);
                    in[4 * i + 2] = (uint8_t)cr; in[4 * i + 3] = (uint8_t)(255 - y - i * 37);
                    g4_yuv_to_pair(in + 4 * i, ref + 8 * i, ref + 8 * i + 4);
                }
                g4_yuv_row(in, out, 8);
                if (memcmp(out, ref, 32)) { if (bad++ < 3) printf("mismatch cb=%u cr=%u y=%u\n", cb, cr, y); }
            }
    for (i = 0; i < 200000; i++) {                     /* random widths and alignments */
        unsigned k;
        w = 1 + (r = r * 1103515245u + 12345u) % 32;
        off = (r >> 8) % 16;
        for (k = 0; k < 64; k++) in[off + k] = (uint8_t)((r = r * 1103515245u + 12345u) >> 16);
        memset(ref, 0xAA, sizeof ref); memset(out, 0xAA, sizeof out);
        for (k = 0; k + 1 < w; k += 2) g4_yuv_to_pair(in + off + 2 * k, ref + off + 4 * k, ref + off + 4 * k + 4);
        if (k < w) { uint8_t t[4]; g4_yuv_to_pair(in + off + 2 * k, ref + off + 4 * k, t); }
        g4_yuv_row(in + off, out + off, w);
        if (memcmp(out, ref, sizeof out)) { if (bad++ < 6) printf("mismatch w=%u off=%u\n", w, off); }
    }
    printf(bad ? "YUV SIMD FAILED (%u)\n" : "YUV row: 16.7M exhaustive + 200k random rows identical\n", bad);
    return bad != 0;
}
