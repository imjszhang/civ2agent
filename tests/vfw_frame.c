#include <windows.h>
#include <vfw.h>
#include <stdio.h>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "Video\\OPENING.AVI";
    PAVIFILE file = NULL;
    PAVISTREAM stream = NULL;
    PGETFRAME frame = NULL;
    LONG nonblack = 0;
    LONG samples = 0;
    int y, x;

    AVIFileInit();
    if (AVIFileOpenA(&file, path, OF_READ, NULL)) {
        printf("open failed %s err=%lu\n", path, GetLastError());
        return 1;
    }
    if (AVIFileGetStream(file, &stream, streamtypeVIDEO, 0)) {
        printf("no video stream\n");
        return 1;
    }
    frame = AVIStreamGetFrameOpen(stream, NULL);
    if (!frame) {
        printf("decompressor failed\n");
        return 1;
    }
    {
        LPBITMAPINFOHEADER bi = (LPBITMAPINFOHEADER)AVIStreamGetFrame(frame, 10);
        BYTE* pixels;
        int stride;
        if (!bi) {
            printf("no frame\n");
            return 1;
        }
        printf("frame %ldx%ld bit=%u compression=%lu\n", bi->biWidth, bi->biHeight, bi->biBitCount, bi->biCompression);
        pixels = (BYTE*)bi + bi->biSize;
        if (bi->biBitCount <= 8) {
            pixels += (bi->biClrUsed ? bi->biClrUsed : (1u << bi->biBitCount)) * 4;
        }
        stride = ((bi->biWidth * (bi->biBitCount / 8) + 3) & ~3);
        if (bi->biHeight < 0) bi->biHeight = -bi->biHeight;
        for (y = 0; y < bi->biHeight; y += 4) {
            BYTE* row = pixels + y * stride;
            for (x = 0; x < bi->biWidth; x += 4) {
                BYTE* p = row + x * (bi->biBitCount / 8);
                samples++;
                if (p[0] | p[1] | p[2]) nonblack++;
            }
        }
        printf("sampled %ld nonblack %ld\n", samples, nonblack);
    }
    AVIStreamGetFrameClose(frame);
    AVIStreamRelease(stream);
    AVIFileRelease(file);
    AVIFileExit();
    return nonblack > 0 ? 0 : 2;
}
