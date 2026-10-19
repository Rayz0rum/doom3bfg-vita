#include <chrono>

// random DXT5 normal-map-like blocks: X in alpha, Y in green, red/blue 0
static void RandomBlock(std::mt19937& rng, byte* b, bool smooth)
{
    std::uniform_int_distribution<int> u8(0, 255), u6(0, 63), u32(0, 0x7fffffff);
    int a0 = u8(rng), a1 = u8(rng), g0 = u6(rng), g1 = u6(rng);
    if (smooth) { // a near-flat, unit-ish area
        a1 = std::clamp(a0 + (u8(rng) % 21) - 10, 0, 255);
        g1 = std::clamp(g0 + (u8(rng) % 5) - 2, 0, 63);
    }
    b[0] = a0; b[1] = a1;
    uint64 bits = ((uint64)u32(rng) << 31) ^ (uint64)u32(rng);
    for (int i = 0; i < 6; i++) b[2 + i] = (byte)(bits >> (8 * i));
    uint16 c0 = (uint16)(g0 << 5), c1 = (uint16)(g1 << 5);
    if (u8(rng) & 1) std::swap(c0, c1);
    b[8] = (byte)c0; b[9] = (byte)(c0 >> 8); b[10] = (byte)c1; b[11] = (byte)(c1 >> 8);
    uint32 ci = (uint32)u32(rng) * 2u + (u8(rng) & 1);
    b[12] = (byte)ci; b[13] = (byte)(ci >> 8); b[14] = (byte)(ci >> 16); b[15] = (byte)(ci >> 24);
}

int main()
{
    std::mt19937 rng(1234);
    // 1. decode: old (accumulate + divide) vs new (means)
    double worstDec = 0; int levelsTested = 0;
    for (int trial = 0; trial < 40; trial++) {
        const int w = 4 << (trial % 6), h = 4 << ((trial / 6) % 5);
        const int bw = w / 4, bh = h / 4;
        std::vector<byte> data(bw * bh * 16);
        for (int i = 0; i < bw * bh; i++) RandomBlock(rng, &data[i * 16], trial & 1);
        const int hw = w / 2, hh = h / 2;
        std::vector<float> a(hw * hh * 4, 0.0f);
        std::vector<int16> b(hw * hh * 3, 0);
        DecodeNormalsToHalf(data.data(), w, h, a.data(), hw, hh);
        // the band split: two halves of the rows
        DecodeBlockRowsToHalf(data.data(), w, 0, bh / 2, b.data(), hw);
        DecodeBlockRowsToHalf(data.data(), w, bh / 2, bh, b.data(), hw);
        for (int i = 0; i < hw * hh; i++) {
            for (int k = 0; k < 3; k++) {
                const double d = fabs(a[i * 4 + k] / a[i * 4 + 3] - b[i * 3 + k] / 32767.0);
                worstDec = std::max(worstDec, d);
            }
        }
        levelsTested++;
    }
    printf("decode: %d levels, worst component difference old vs new %.3g\n", levelsTested, worstDec);

    // 2. the blue table vs the exact formula
    if (!toksvigLutReady) ToksvigInitLut();
    double worstLut = 0;
    for (int i = 0; i <= 200000; i++) {
        const float len = 0.85f + 0.15f * i / 200000.0f;
        const double d = fabs(ToksvigBlueOfLen2(len * len) - ToksvigBlue(len));
        worstLut = std::max(worstLut, d);
    }
    printf("blue table: worst difference from the exact value %.3f/255 (rounding included)\n", worstLut);
    // the endpoints: |Na| 1 -> 0, |Na| 0.8696 (s' 4) -> 255
    printf("blue at |Na| 1: %d, 0.95: %d (exact %.2f), 0.8696: %d\n", ToksvigBlueOfLen2(1.0f), ToksvigBlueOfLen2(0.9025f), ToksvigBlue(0.95f), ToksvigBlueOfLen2(0.8696f * 0.8696f));

    // 3. the fit: old (float) vs new (whole numbers), same blocks and targets
    double errOld = 0, errNew = 0; int cOld = 0, cNew = 0, gOld = 0, gNew = 0, diffBlocks = 0;
    int greenBitsChanged = 0;
    for (int trial = 0; trial < 200000; trial++) {
        byte blk[16];
        RandomBlock(rng, blk, trial & 1);
        if (trial % 20 == 0) { // flat green blocks
            blk[10] = blk[8]; blk[11] = blk[9];
            uint16 c = blk[8] | (blk[9] << 8); (void)c;
        }
        float tf[16]; int ti[16]; bool in[16];
        const int base = rng() % 256, span = rng() % 64;
        for (int t = 0; t < 16; t++) {
            in[t] = true;
            ti[t] = std::clamp(base + (int)(rng() % (span + 1)) - span / 2, 0, 255);
            tf[t] = (float)ti[t];
        }
        byte b1[16], b2[16];
        memcpy(b1, blk, 16); memcpy(b2, blk, 16);
        errOld += OldFitBlue(b1, tf, in, &cOld, &gOld);
        errNew += FitBlue(b2, ti, in, &cNew, &gNew);
        if (memcmp(b1, b2, 16) != 0) diffBlocks++;
        // green: decode both before/after
        int g0[4], g1[4];
        GreenPalette(blk[8] | (blk[9] << 8), blk[10] | (blk[11] << 8), g0);
        GreenPalette(b2[8] | (b2[9] << 8), b2[10] | (b2[11] << 8), g1);
        const uint32 ib = blk[12] | (blk[13] << 8) | (blk[14] << 16) | ((uint32)blk[15] << 24);
        const uint32 ia = b2[12] | (b2[13] << 8) | (b2[14] << 16) | ((uint32)b2[15] << 24);
        for (int t = 0; t < 16; t++) {
            if (g0[(ib >> (2 * t)) & 3] != g1[(ia >> (2 * t)) & 3]) { greenBitsChanged++; break; }
        }
        const bool gtBefore = (blk[8] | (blk[9] << 8)) > (blk[10] | (blk[11] << 8));
        const bool gtAfter = (b2[8] | (b2[9] << 8)) > (b2[10] | (b2[11] << 8));
        if (gtBefore != gtAfter) { printf("FAIL order\n"); return 1; }
        if (memcmp(blk, b2, 2) != 0 || memcmp(blk + 2, b2 + 2, 6) != 0) { printf("FAIL alpha touched\n"); return 1; }
    }
    printf("fit: old %.3f/255 per texel (%d texels, %d refused), new %.3f/255 (%d texels, %d refused); %d of 200000 blocks differ; blocks whose decoded green changed: %d (must be 0)\n",
        errOld / std::max(cOld, 1), cOld, gOld, errNew / std::max(cNew, 1), cNew, gNew, diffBlocks, greenBitsChanged);
    {
        const int w = 256, h = 256, bw = 64, bh = 64;
        std::vector<byte> data(bw * bh * 16);
        for (int i = 0; i < bw * bh; i++) RandomBlock(rng, &data[i * 16], true);
        std::vector<float> a(128 * 128 * 4);
        std::vector<int16> q(128 * 128 * 3);
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int r = 0; r < 50; r++) { std::fill(a.begin(), a.end(), 0.0f); DecodeNormalsToHalf(data.data(), w, h, a.data(), 128, 128); }
        auto t1 = std::chrono::high_resolution_clock::now();
        for (int r = 0; r < 50; r++) DecodeBlockRowsToHalf(data.data(), w, 0, bh, q.data(), 128);
        auto t2 = std::chrono::high_resolution_clock::now();
        printf("decode 256x256: old %.3f ms, new %.3f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count() / 50, std::chrono::duration<double, std::milli>(t2 - t1).count() / 50);
        // fit timing over 4096 blocks
        std::vector<byte> blocks(4096 * 16); for (int i = 0; i < 4096; i++) RandomBlock(rng, &blocks[i * 16], true);
        float tf[16]; int ti[16]; bool in[16]; for (int t = 0; t < 16; t++) { in[t] = true; ti[t] = 40 + t; tf[t] = (float)ti[t]; }
        int c = 0, g = 0; std::vector<byte> w1 = blocks, w2 = blocks;
        auto t3 = std::chrono::high_resolution_clock::now();
        for (int r = 0; r < 20; r++) for (int i = 0; i < 4096; i++) OldFitBlue(&w1[i * 16], tf, in, &c, &g);
        auto t4 = std::chrono::high_resolution_clock::now();
        for (int r = 0; r < 20; r++) for (int i = 0; i < 4096; i++) FitBlue(&w2[i * 16], ti, in, &c, &g);
        auto t5 = std::chrono::high_resolution_clock::now();
        printf("fit 4096 blocks: old %.3f ms, new %.3f ms\n", std::chrono::duration<double, std::milli>(t4 - t3).count() / 20, std::chrono::duration<double, std::milli>(t5 - t4).count() / 20);
    }
    return greenBitsChanged == 0 ? 0 : 1;
}
