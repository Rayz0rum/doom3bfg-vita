// Checks the streamed sound path's MS ADPCM decoder (neo/sound/OpenAL/AL_ADPCM.h)
// against the loop it replaced, bit for bit, on random blocks - mono and
// stereo, the standard coefficient table and random ones, every block size
// the data uses and odd ones.
//   g++ -O2 -o adpcm_test tools/vita/adpcm_test.cpp && ./adpcm_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "../../neo/sound/OpenAL/AL_ADPCM.h"

// the loop from idSoundSample_OpenAL::DecodeBlock before AL_ADPCM.h
static int OldDecode(const unsigned char* src, int bytes, short* dst, int ch, int framesPerBlock, const short* coefPairs)
{
    static const int adaptive[16] = {
        230, 230, 230, 230, 307, 409, 512, 614,
        768, 614, 512, 409, 307, 230, 230, 230
    };
    if (ch < 1 || ch > 2 || bytes < 7 * ch) {
        return 0;
    }
    int coef1[2], coef2[2], delta[2], s1[2], s2[2];
    const unsigned char* p = src;
    for (int c = 0; c < ch; c++) {
        int pred = *p++;
        pred = pred < 0 ? 0 : (pred > 6 ? 6 : pred);
        coef1[c] = coefPairs[pred * 2 + 0];
        coef2[c] = coefPairs[pred * 2 + 1];
    }
    for (int c = 0; c < ch; c++, p += 2) {
        delta[c] = p[0] | (p[1] << 8);
    }
    for (int c = 0; c < ch; c++, p += 2) {
        s1[c] = (short)(p[0] | (p[1] << 8));
    }
    for (int c = 0; c < ch; c++, p += 2) {
        s2[c] = (short)(p[0] | (p[1] << 8));
    }
    short* out = dst;
    for (int c = 0; c < ch; c++) {
        *out++ = (short)s2[c];
    }
    for (int c = 0; c < ch; c++) {
        *out++ = (short)s1[c];
    }
    const int maxNibbles = (framesPerBlock - 2) * ch;
    int nibbleBytes = bytes - 7 * ch;
    if (nibbleBytes > maxNibbles / 2) {
        nibbleBytes = maxNibbles / 2;
    }
    const unsigned char* end = p + nibbleBytes;
    int c = 0;
    while (p < end) {
        const int b = *p++;
        for (int half = 0; half < 2; half++) {
            const int nib = half == 0 ? (b >> 4) : (b & 15);
            int sample = ((s1[c] * coef1[c]) + (s2[c] * coef2[c])) / 256;
            sample += delta[c] * ((nib & 8) ? (nib - 16) : nib);
            if (sample > 32767) {
                sample = 32767;
            } else if (sample < -32768) {
                sample = -32768;
            }
            int d = (delta[c] * adaptive[nib]) / 256;
            delta[c] = (d < 16 ? 16 : d) & 0xFFFF;
            s2[c] = s1[c];
            s1[c] = sample;
            *out++ = (short)sample;
            c ^= (ch - 1);
        }
    }
    return (int)(out - dst) / ch;
}

int main()
{
    static const short standard[14] = { 256, 0, 512, -256, 0, 0, 192, 64, 240, 0, 460, -208, 392, -232 };
    std::mt19937 rng(12345);
    static unsigned char block[8192];
    static short a[16384], b[16384];
    long long blocks = 0, samples = 0, mismatches = 0;
    const int sizes[] = { 1024, 512, 256, 2048, 36, 37, 70, 71, 7, 14, 15 };
    for (int iter = 0; iter < 400000; iter++) {
        const int ch = 1 + (iter & 1);
        short coefs[14];
        if (iter % 3 == 0) {
            for (int i = 0; i < 14; i++) {
                coefs[i] = (short)((int)(rng() % 4097) - 2048);
            }
        } else {
            memcpy(coefs, standard, sizeof(coefs));
        }
        const int blockAlign = sizes[rng() % (sizeof(sizes) / sizeof(sizes[0]))] * ch;
        const int framesPerBlock = (blockAlign - 7 * ch) * 2 / ch + 2;
        int bytes = blockAlign;
        if (rng() % 8 == 0) {
            bytes = (int)(rng() % (blockAlign + 1)); // a short last block
        }
        for (int i = 0; i < bytes; i++) {
            block[i] = (unsigned char)rng();
        }
        if (rng() % 4 == 0) {
            // extreme states: big step sizes and samples at the rails
            if (bytes >= 7 * ch) {
                for (int c = 0; c < ch; c++) {
                    block[ch + c * 2] = 0xFF;
                    block[ch + c * 2 + 1] = (unsigned char)(0x80 | (rng() & 0x7F));
                }
            }
        }
        memset(a, 0x55, sizeof(a));
        memset(b, 0x55, sizeof(b));
        const int na = OldDecode(block, bytes, a, ch, framesPerBlock, coefs);
        const int nb = ADPCM_DecodeBlock(block, bytes, b, ch, framesPerBlock, coefs);
        blocks++;
        samples += na * ch;
        if (na != nb || memcmp(a, b, sizeof(short) * (na > 0 ? na * ch : 0)) != 0) {
            if (mismatches++ < 10) {
                printf("MISMATCH block %lld: ch %d bytes %d frames %d vs %d\n", blocks, ch, bytes, na, nb);
            }
        }
    }
    printf("%lld blocks, %lld samples compared, %lld mismatches\n", blocks, samples, mismatches);
    return mismatches ? 1 : 0;
}
