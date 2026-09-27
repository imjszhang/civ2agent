#include <windows.h>
#include <vfw.h>
#include <stdio.h>

static long try_ex(HIC hic, LPBITMAPINFOHEADER src, void* bits, LPBITMAPINFOHEADER dst, void* out,
                    int dw, int dh) {
    return ICDecompressEx(hic, 0, src, bits, 0, 0, src->biWidth, src->biHeight, dst, out, 0, 0, dw, dh);
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "Video\\OPENING.AVI";
    PAVIFILE file = NULL;
    PAVISTREAM stream = NULL;
    LONG fmt = 0;
    LPBITMAPINFOHEADER biIn = NULL;
    void* bits = NULL;
    LONG bitsLen = 0;
    HIC hic;
    DWORD outSize;
    BITMAPINFOHEADER biOut;
    void* out;
    long r1, r2;

    AVIFileInit();
    if (AVIFileOpenA(&file, path, OF_READ, NULL) || AVIFileGetStream(file, &stream, streamtypeVIDEO, 0)) {
        printf("open failed\n");
        return 1;
    }
    AVIStreamReadFormat(stream, 0, NULL, &fmt);
    biIn = (LPBITMAPINFOHEADER)malloc(fmt);
    AVIStreamReadFormat(stream, 0, biIn, &fmt);
    {
        LONG n = AVIStreamLength(stream);
        LONG bad = 0, first = -1, firstCode = 0;
        printf("frames %ld\n", n);
        memset(&biOut, 0, sizeof(biOut));
        biOut.biSize = sizeof(biOut);
        biOut.biWidth = biIn->biWidth;
        biOut.biHeight = biIn->biHeight;
        biOut.biPlanes = 1;
        biOut.biBitCount = 8;
        biOut.biCompression = BI_RGB;
        hic = ICLocate(ICTYPE_VIDEO, biIn->biCompression, biIn, NULL, ICMODE_DECOMPRESS);
        ICDecompressBegin(hic, biIn, &biOut);
        out = malloc((size_t)biIn->biWidth * (biIn->biHeight < 0 ? -biIn->biHeight : biIn->biHeight) * 4 + 64);
        for (LONG f = 0; f < n; f++) {
            LONG got = 0;
            if (AVIStreamRead(stream, f, 1, NULL, 0, &bitsLen, NULL)) continue;
            bits = realloc(bits, bitsLen ? bitsLen : 1);
            if (AVIStreamRead(stream, f, 1, bits, bitsLen, &got, NULL)) continue;
            r1 = try_ex(hic, biIn, bits, &biOut, out, biIn->biWidth, biIn->biHeight < 0 ? -biIn->biHeight : biIn->biHeight);
            if (r1 != 0 && r1 != 1) {
                bad++;
                if (first < 0) { first = f; firstCode = r1; }
            }
        }
        printf("bad frames %ld first %ld code %ld\n", bad, first, firstCode);
        ICDecompressEnd(hic);
        ICClose(hic);
        return 0;
    }
    AVIStreamRead(stream, 10, 1, NULL, 0, &bitsLen, NULL);
    bits = malloc(bitsLen);
    if (AVIStreamRead(stream, 10, 1, bits, bitsLen, NULL, NULL)) {
        printf("read frame failed\n");
        return 1;
    }
    printf("src %ldx%ld comp=%lu bit=%u frameBytes=%ld\n", biIn->biWidth, biIn->biHeight, biIn->biCompression, biIn->biBitCount, bitsLen);

    hic = ICLocate(ICTYPE_VIDEO, biIn->biCompression, biIn, NULL, ICMODE_DECOMPRESS);
    if (!hic) {
        printf("ICLocate failed\n");
        return 1;
    }
    out = malloc((size_t)biIn->biWidth * biIn->biHeight * 16);
    memset(&biOut, 0, sizeof(biOut));
    biOut.biSize = sizeof(biOut);
    biOut.biWidth = biIn->biWidth;
    biOut.biHeight = -biIn->biHeight;
    biOut.biPlanes = 1;
    biOut.biBitCount = 8;
    biOut.biCompression = BI_RGB;
    printf("neg height query=%ld ", ICDecompressQuery(hic, biIn, &biOut));
    if (ICDecompressBegin(hic, biIn, &biOut) == ICERR_OK) {
        printf("ex=%ld\n", try_ex(hic, biIn, bits, &biOut, out, biIn->biWidth, biIn->biHeight > 0 ? biIn->biHeight : -biIn->biHeight));
        ICDecompressEnd(hic);
    } else printf("begin failed %ld\n", ICDecompressBegin(hic, biIn, &biOut));
    for (int bit = 8; bit <= 32; bit += 8) {
        memset(&biOut, 0, sizeof(biOut));
        biOut.biSize = sizeof(biOut);
        biOut.biWidth = biIn->biWidth;
        biOut.biHeight = biIn->biHeight;
        biOut.biPlanes = 1;
        biOut.biBitCount = (WORD)bit;
        biOut.biCompression = BI_RGB;
        printf("bit %d query=%ld ", bit, ICDecompressQuery(hic, biIn, &biOut));
        if (ICDecompressBegin(hic, biIn, &biOut) == ICERR_OK) {
            r1 = try_ex(hic, biIn, bits, &biOut, out, biIn->biWidth, biIn->biHeight);
            biOut.biWidth = biIn->biWidth * 2;
            biOut.biHeight = biIn->biHeight * 2;
            r2 = try_ex(hic, biIn, bits, &biOut, out, biOut.biWidth, biOut.biHeight);
            printf("ex=%ld scaled=%ld\n", r1, r2);
            ICDecompressEnd(hic);
        } else {
            printf("begin failed\n");
        }
    }
    ICDecompressEnd(hic);
    ICClose(hic);
    return 0;
}
