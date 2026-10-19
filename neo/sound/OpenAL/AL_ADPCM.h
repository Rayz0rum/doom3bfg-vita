/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
MS ADPCM block decoding for the streamed sound path (AL_SoundStream.cpp).

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================

The same arithmetic as MS_ADPCM_decode (and the loop it replaces), sample
for sample: the predictor sum divided by 256 with C's truncation, the nibble
sign-extended, the result saturated to 16 bits, the step size scaled by the
adaptation table and floored at 16. What changed is only how it is laid out
for the CPU: one loop per channel count with the state in registers (the
old one indexed per-channel arrays on every sample, so they lived in memory),
both nibbles of a byte decoded straight-line, and a saturation GCC turns
into one ssat. On the Vita the decode was the bulk of the sound update -
5-7 ms a frame in the benchmark's fights, on the game/draw thread.
tools/vita/adpcm_test.cpp checks it against the old loop, bit for bit.

Free of engine headers so the test can include it as is.

===========================================================================
*/
#ifndef __AL_ADPCM_H__
#define __AL_ADPCM_H__

// GCC turns this into one ssat on the Vita, and the test runs this very code
static inline int ADPCM_Saturate16(int v)
{
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

// one nibble of one channel
#define ADPCM_STEP(nib, s1, s2, delta, coef1, coef2, out) \
    do { \
        int sample_ = ((s1) * (coef1) + (s2) * (coef2)) / 256; \
        sample_ += (delta) * (((nib) ^ 8) - 8); \
        sample_ = ADPCM_Saturate16(sample_); \
        const int d_ = ((delta) * adpcmAdaptive[(nib)]) >> 8; /* delta >= 0: the same as / 256 */ \
        (delta) = (d_ < 16 ? 16 : d_) & 0xFFFF; \
        (s2) = (s1); \
        (s1) = sample_; \
        *(out)++ = (short)sample_; \
    } while (0)

/*
ADPCM_DecodeBlock

One MS ADPCM block to interleaved 16-bit PCM. coefPairs is the format's
seven (coef1, coef2) pairs, in that order. Returns the frames written; dst
must hold framesPerBlock * ch samples. 0 for a block that is too short or a
channel count other than 1 or 2.
*/
static inline int ADPCM_DecodeBlock(const unsigned char* src, int bytes, short* dst, int ch, int framesPerBlock, const short* coefPairs)
{
    static const int adpcmAdaptive[16] = {
        230, 230, 230, 230, 307, 409, 512, 614,
        768, 614, 512, 409, 307, 230, 230, 230
    };

    if (ch < 1 || ch > 2 || bytes < 7 * ch) {
        return 0;
    }

    const unsigned char* p = src;
    short* out = dst;
    const int maxNibbles = (framesPerBlock - 2) * ch;
    int nibbleBytes = bytes - 7 * ch;
    if (nibbleBytes > maxNibbles / 2) {
        nibbleBytes = maxNibbles / 2;
    }

    if (ch == 1) {
        int pred = p[0];
        pred = pred < 0 ? 0 : (pred > 6 ? 6 : pred);
        const int coef1 = coefPairs[pred * 2 + 0];
        const int coef2 = coefPairs[pred * 2 + 1];
        int delta = p[1] | (p[2] << 8); // unsigned, as in MS_ADPCM_decode
        int s1 = (short)(p[3] | (p[4] << 8));
        int s2 = (short)(p[5] | (p[6] << 8));
        p += 7;
        *out++ = (short)s2;
        *out++ = (short)s1;
        const unsigned char* end = p + nibbleBytes;
        while (p < end) {
            const int b = *p++;
            const int hi = b >> 4;
            const int lo = b & 15;
            ADPCM_STEP(hi, s1, s2, delta, coef1, coef2, out);
            ADPCM_STEP(lo, s1, s2, delta, coef1, coef2, out);
        }
        return (int)(out - dst);
    }

    // stereo: the high nibble of each byte is the left channel, the low one the right
    int predL = p[0], predR = p[1];
    predL = predL < 0 ? 0 : (predL > 6 ? 6 : predL);
    predR = predR < 0 ? 0 : (predR > 6 ? 6 : predR);
    const int coef1L = coefPairs[predL * 2 + 0], coef2L = coefPairs[predL * 2 + 1];
    const int coef1R = coefPairs[predR * 2 + 0], coef2R = coefPairs[predR * 2 + 1];
    int deltaL = p[2] | (p[3] << 8);
    int deltaR = p[4] | (p[5] << 8);
    int s1L = (short)(p[6] | (p[7] << 8));
    int s1R = (short)(p[8] | (p[9] << 8));
    int s2L = (short)(p[10] | (p[11] << 8));
    int s2R = (short)(p[12] | (p[13] << 8));
    p += 14;
    *out++ = (short)s2L;
    *out++ = (short)s2R;
    *out++ = (short)s1L;
    *out++ = (short)s1R;
    const unsigned char* end = p + nibbleBytes;
    while (p < end) {
        const int b = *p++;
        const int hi = b >> 4;
        const int lo = b & 15;
        ADPCM_STEP(hi, s1L, s2L, deltaL, coef1L, coef2L, out);
        ADPCM_STEP(lo, s1R, s2R, deltaR, coef1R, coef2R, out);
    }
    return (int)(out - dst) / 2;
}

#undef ADPCM_STEP

#endif /* !__AL_ADPCM_H__ */
