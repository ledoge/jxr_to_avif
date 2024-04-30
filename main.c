// adapted from avif-example-encode.c, see libavif license in LICENSE-THIRD-PARTY
// original copyright notice follows

// Copyright 2020 Joe Drago. All rights reserved.
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <wincodec.h>
#include <math.h>
#include <stdint.h>

#include "avif.h"

#define INTERMEDIATE_BITS 16  // bit depth of the integer texture given to the encoder
#define DEFAULT_SPEED 6  // 6 is default speed of the command line encoder, so it should be a good value?
#define USE_TILING AVIF_TRUE  // slightly larger file size, but faster encode and decode

#define TARGET_BITS 12  // bit depth of the output, should be 10 or 12
#define TARGET_FORMAT AVIF_PIXEL_FORMAT_YUV444
//#define TARGET_RGB  // uncomment to output RGB instead of YUV (much larger file size)

#define MAXCLL_PERCENTILE 0.9999  // comment out to calculate true MaxCLL instead of top percentile

static float m1 = 1305 / 8192.f;
static float m2 = 2523 / 32.f;
static float c1 = 107 / 128.f;
static float c2 = 2413 / 128.f;
static float c3 = 2392 / 128.f;

float pq_inv_eotf(float y) {
    return powf((c1 + c2 * powf(y, m1)) / (1 + c3 * powf(y, m1)), m2);
}

static const float scrgb_to_bt2100[3][3] = {
        {2939026994.L / 585553224375.L, 9255011753.L / 3513319346250.L, 173911579.L / 501902763750.L},
        {76515593.L / 138420033750.L,   6109575001.L / 830520202500.L,  75493061.L / 830520202500.L},
        {12225392.L / 93230009375.L,    1772384008.L / 2517210253125.L, 18035212433.L / 2517210253125.L},
};

void matrixVectorMult(const float in[3], float out[3], const float matrix[3][3]) {
    for (int i = 0; i < 3; i++) {
        float res = 0;
        for (int j = 0; j < 3; j++) {
            res += matrix[i][j] * in[j];
        }
        out[i] = res;
    }
}

float saturate(float x) {
    return min(1, max(x, 0));
}

typedef struct ThreadData {
    uint8_t *pixels;
    uint16_t *converted;
    uint32_t width;
    uint32_t start;
    uint32_t stop;
    double sumOfMaxComp;
#ifdef MAXCLL_PERCENTILE
    uint32_t *nitCounts;
#endif
    uint16_t maxNits;
    uint8_t bytesPerColor;
} ThreadData;

DWORD WINAPI ThreadFunc(LPVOID lpParam) {
    ThreadData *d = (ThreadData *) lpParam;
    uint8_t *pixels = d->pixels;
    uint8_t bytesPerColor = d->bytesPerColor;
    uint16_t *converted = d->converted;
    uint32_t width = d->width;
    uint32_t start = d->start;
    uint32_t stop = d->stop;

    float maxMaxComp = 0;
    double sumOfMaxComp = 0;

    for (uint32_t i = start; i < stop; i++) {
        for (uint32_t j = 0; j < width; j++) {
            float bt2020[3];

            if (bytesPerColor == 4) {
                matrixVectorMult((float *) pixels + i * 4 * width + 4 * j, bt2020, scrgb_to_bt2100);
            } else {
                float cur[3];
                _Float16 *cur16 = (_Float16 *) pixels + i * 4 * width + 4 * j;
                for (int k = 0; k < 3; k++) {
                    cur[k] = (float) cur16[k];
                }
                matrixVectorMult(cur, bt2020, scrgb_to_bt2100);
            }

            for (int k = 0; k < 3; k++) {
                bt2020[k] = saturate(bt2020[k]);
            }

            float maxComp = max(bt2020[0], max(bt2020[1], bt2020[2]));

#ifdef MAXCLL_PERCENTILE
            uint32_t nits = (uint32_t) roundf(maxComp * 10000);
            d->nitCounts[nits]++;
#endif
            if (maxComp > maxMaxComp) {
                maxMaxComp = maxComp;
            }

            sumOfMaxComp += maxComp;

            for (int k = 0; k < 3; k++) {
                converted[(size_t) 3 * width * i + (size_t) 3 * j + k] = (uint16_t) roundf(
                        pq_inv_eotf(bt2020[k]) * ((1 << INTERMEDIATE_BITS) - 1));
            }
        }
    }

    d->maxNits = (uint16_t) roundf(maxMaxComp * 10000);
    d->sumOfMaxComp = sumOfMaxComp;

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc <= 1 || argc > 3 == strcmp(argv[1], "--speed") || argc > 5) {
        fprintf(stderr, "jxr_to_avif [--speed n] input.jxr [output.avif]\n");
        return 1;
    }

    int speed = DEFAULT_SPEED;
    LPWSTR inputFile;
    LPWSTR outputFile = L"output.avif";

    {
        LPWSTR *szArglist;
        int nArgs;

        szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
        if (NULL == szArglist) {
            fprintf(stderr, "CommandLineToArgvW failed\n");
            return 1;
        }

        int rest = 1;

        if (!strcmp("--speed", argv[1])) {
            speed = atoi(argv[2]);
            if (speed < AVIF_SPEED_SLOWEST || speed > AVIF_SPEED_FASTEST) {
                fprintf(stderr, "Speed must be in range [%d, %d]", AVIF_SPEED_SLOWEST, AVIF_SPEED_FASTEST);
                return 1;
            }
            rest += 2;
        }

        inputFile = szArglist[rest + 0];

        if (rest + 1 < argc) {
            outputFile = szArglist[rest + 1];
        }
    }

    // Create a decoder
    IWICBitmapDecoder *pDecoder = NULL;

    // Initialize COM
    CoInitialize(NULL);

    // The factory pointer
    IWICImagingFactory *pFactory = NULL;

    // Create the COM imaging factory
    HRESULT hr = CoCreateInstance(
            &CLSID_WICImagingFactory,
            NULL,
            CLSCTX_INPROC_SERVER,
            &IID_IWICImagingFactory,
            (void **) &pFactory);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to create WIC imaging factory\n");
        return 1;
    }

    hr = pFactory->lpVtbl->CreateDecoderFromFilename(
            pFactory,
            inputFile,                       // Image to be decoded
            NULL,                            // Do not prefer a particular vendor
            GENERIC_READ,                    // Desired read access to the file
            WICDecodeMetadataCacheOnDemand,  // Cache metadata when needed
            &pDecoder                        // Pointer to the decoder
    );

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to open file\n");
        return 1;
    }

    // Retrieve the first frame of the image from the decoder
    IWICBitmapFrameDecode *pFrame = NULL;

    hr = pDecoder->lpVtbl->GetFrame(pDecoder, 0, &pFrame);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get frame\n");
        return 1;
    }

    IWICBitmapSource *pBitmapSource = NULL;

    hr = pFrame->lpVtbl->QueryInterface(pFrame, &IID_IWICBitmapSource, (void **) &pBitmapSource);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get IWICBitmapSource\n");
        return 1;
    }

    WICPixelFormatGUID pixelFormat;

    hr = pBitmapSource->lpVtbl->GetPixelFormat(pBitmapSource, &pixelFormat);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get pixel format\n");
        return 1;
    }

    uint8_t bytesPerColor;

    if (IsEqualGUID((void *) &pixelFormat, (void *) &GUID_WICPixelFormat128bppRGBAFloat)) {
        bytesPerColor = 4;
    } else if (IsEqualGUID((void *) &pixelFormat, (void *) &GUID_WICPixelFormat64bppRGBAHalf)) {
        bytesPerColor = 2;
    } else {
        fprintf(stderr, "Unsupported pixel format\n");
        return 1;
    }

    uint32_t width, height;

    hr = pBitmapSource->lpVtbl->GetSize(pBitmapSource, &width, &height);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get size\n");
        return 1;
    }

    uint16_t *converted = malloc(sizeof(uint16_t) * width * height * 3);

    if (converted == NULL) {
        fprintf(stderr, "Failed to allocate converted pixels\n");
        return 1;
    }

    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    uint32_t numThreads = min(systemInfo.dwNumberOfProcessors, 64);
    printf("Using %d threads\n", numThreads);

    puts("Converting pixels to BT.2100 PQ...");

    uint16_t maxCLL, maxPALL;

    {

        UINT cbStride = width * bytesPerColor * 4;
        UINT cbBufferSize = cbStride * height;

        uint8_t *pixels = malloc(cbBufferSize);

        if (converted == NULL) {
            fprintf(stderr, "Failed to allocate float pixels\n");
            return 1;
        }

        WICRect rc;
        rc.Y = 0;
        rc.X = 0;
        rc.Width = (int) width;
        rc.Height = (int) height;
        hr = pBitmapSource->lpVtbl->CopyPixels(pBitmapSource,
                                               &rc,
                                               cbStride,
                                               cbBufferSize,
                                               pixels);

        if (FAILED(hr)) {
            fprintf(stderr, "Failed to copy pixels\n");
            exit(1);
        }

        uint32_t convThreads = numThreads;

        uint32_t chunkSize = height / convThreads;

        if (chunkSize == 0) {
            convThreads = height;
            chunkSize = 1;
        }

        HANDLE hThreadArray[convThreads];
        ThreadData *threadData[convThreads];
        DWORD dwThreadIdArray[convThreads];

        for (uint32_t i = 0; i < convThreads; i++) {
            threadData[i] = malloc(sizeof(ThreadData));
            threadData[i]->pixels = pixels;
            threadData[i]->bytesPerColor = bytesPerColor;
            threadData[i]->converted = converted;
            threadData[i]->width = width;
            threadData[i]->start = i * chunkSize;
            if (i != convThreads - 1) {
                threadData[i]->stop = (i + 1) * chunkSize;
            } else {
                threadData[i]->stop = height;
            }

#ifdef MAXCLL_PERCENTILE
            threadData[i]->nitCounts = calloc(10000, sizeof(typeof(threadData[i]->nitCounts[0])));
#endif

            HANDLE hThread = CreateThread(
                    NULL,                   // default security attributes
                    0,                      // use default stack size
                    ThreadFunc,       // thread function name
                    threadData[i],          // argument to thread function
                    0,                      // use default creation flags
                    &dwThreadIdArray[i]);   // returns the thread identifier

            if (hThread) {
                hThreadArray[i] = hThread;
            } else {
                fprintf(stderr, "Failed to create thread\n");
                return 1;
            }
        }

        WaitForMultipleObjects(convThreads, hThreadArray, TRUE, INFINITE);

        maxCLL = 0;
        double sumOfMaxComp = 0;

        for (uint32_t i = 0; i < convThreads; i++) {
            CloseHandle(hThreadArray[i]);

            uint16_t tMaxNits = threadData[i]->maxNits;
            if (tMaxNits > maxCLL) {
                maxCLL = tMaxNits;
            }

            sumOfMaxComp += threadData[i]->sumOfMaxComp;
        }

#ifdef MAXCLL_PERCENTILE
        uint16_t currentIdx = maxCLL;
        uint64_t count = 0;
        uint64_t countTarget = (uint64_t) round((1 - MAXCLL_PERCENTILE) * (double) ((uint64_t) width * height));
        while (1) {
            for (uint32_t i = 0; i < convThreads; i++) {
                count += threadData[i]->nitCounts[currentIdx];
            }
            if (count >= countTarget) {
                maxCLL = currentIdx;
                break;
            }
            currentIdx--;
        }
#endif

        for (uint32_t i = 0; i < convThreads; i++) {
#ifdef MAXCLL_PERCENTILE
            free(threadData[i]->nitCounts);
#endif
            free(threadData[i]);
        }

        maxPALL = (uint16_t) round(10000 * (sumOfMaxComp / (double) ((uint64_t) width * height)));

        free(pixels);
    }

    int returnCode = 1;
    avifEncoder *encoder = NULL;
    avifRWData avifOutput = AVIF_DATA_EMPTY;
    avifRGBImage rgb;
    memset(&rgb, 0, sizeof(rgb));

    avifImage *image = avifImageCreate(width, height, TARGET_BITS,
                                       TARGET_FORMAT); // these values dictate what goes into the final AVIF
    if (!image) {
        fprintf(stderr, "Out of memory\n");
        goto cleanup;
    }
    // Configure image here: (see avif/avif.h)
    // * colorPrimaries
    // * transferCharacteristics
    // * matrixCoefficients
    // * avifImageSetProfileICC()
    // * avifImageSetMetadataExif()
    // * avifImageSetMetadataXMP()
    // * yuvRange
    // * alphaPremultiplied
    // * transforms (transformFlags, pasp, clap, irot, imir)
    image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
    image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084;

#ifdef TARGET_RGB
    image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
#else
    image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
#endif

    printf("Computed HDR metadata: %u MaxCLL, %u MaxPALL\n", maxCLL, maxPALL);

    image->clli.maxCLL = maxCLL;
    image->clli.maxPALL = maxPALL;

    // If you have RGB(A) data you want to encode, use this path
    printf("Doing AVIF encoding...\n");

    avifRGBImageSetDefaults(&rgb, image);
    // Override RGB(A)->YUV(A) defaults here:
    //   depth, format, chromaDownsampling, avoidLibYUV, ignoreAlpha, alphaPremultiplied, etc.
    rgb.format = AVIF_RGB_FORMAT_RGB;
    rgb.depth = INTERMEDIATE_BITS;
    rgb.pixels = (void *) converted;
    rgb.rowBytes = 3 * sizeof(uint16_t) * width;

    avifResult convertResult = avifImageRGBToYUV(image, &rgb);
    if (convertResult != AVIF_RESULT_OK) {
        fprintf(stderr, "Failed to convert to YUV(A): %s\n", avifResultToString(convertResult));
        goto cleanup;
    }

    free(rgb.pixels);

    encoder = avifEncoderCreate();
    if (!encoder) {
        fprintf(stderr, "Out of memory\n");
        goto cleanup;
    }
    // Configure your encoder here (see avif/avif.h):
    // * maxThreads
    // * quality
    // * qualityAlpha
    // * tileRowsLog2
    // * tileColsLog2
    // * speed
    // * keyframeInterval
    // * timescale
    encoder->quality = AVIF_QUALITY_LOSSLESS;
    encoder->qualityAlpha = AVIF_QUALITY_LOSSLESS;
    encoder->speed = speed;
    encoder->maxThreads = (int) numThreads;
    encoder->autoTiling = USE_TILING;

    // Call avifEncoderAddImage() for each image in your sequence
    // Only set AVIF_ADD_IMAGE_FLAG_SINGLE if you're not encoding a sequence
    // Use avifEncoderAddImageGrid() instead with an array of avifImage* to make a grid image
    avifResult addImageResult = avifEncoderAddImage(encoder, image, 1, AVIF_ADD_IMAGE_FLAG_SINGLE);
    if (addImageResult != AVIF_RESULT_OK) {
        fprintf(stderr, "Failed to add image to encoder: %s\n", avifResultToString(addImageResult));
        goto cleanup;
    }

    avifResult finishResult = avifEncoderFinish(encoder, &avifOutput);
    if (finishResult != AVIF_RESULT_OK) {
        fprintf(stderr, "Failed to finish encode: %s\n", avifResultToString(finishResult));
        goto cleanup;
    }

    printf("Encode success: %zu total bytes\n", avifOutput.size);

    FILE *f = _wfopen(outputFile, L"wb");
    size_t bytesWritten = fwrite(avifOutput.data, 1, avifOutput.size, f);
    fclose(f);
    if (bytesWritten != avifOutput.size) {
        fprintf(stderr, "Failed to write %zu bytes\n", avifOutput.size);
        goto cleanup;
    }
    printf("Wrote: %ls\n", outputFile);

    returnCode = 0;
    cleanup:
    if (image) {
        avifImageDestroy(image);
    }
    if (encoder) {
        avifEncoderDestroy(encoder);
    }
    avifRWDataFree(&avifOutput);
    return returnCode;
}
