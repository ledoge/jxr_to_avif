// adapted from avif-example-encode.c, see libavif license in LICENSE-THIRD-PARTY
// original copyright notice follows

// Copyright 2020 Joe Drago. All rights reserved.
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <wincodec.h>
#include <math.h>
#include <stdint.h>
#include <avif/avif.h>

#define INTERMEDIATE_BITS 16  // bit depth of the integer texture given to the encoder
#define ENCODER_SPEED 6  // 6 is default speed of the command line encoder, so it should be a good value?
#define USE_TILING AVIF_TRUE  // slightly larger file size, but faster encode and decode

#define TARGET_BITS 12  // bit depth of the output, should be 10 or 12
#define TARGET_FORMAT AVIF_PIXEL_FORMAT_YUV444
//#define TARGET_RGB  // uncomment to output RGB instead of YUV (much larger file size)

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

void matrixVectorMult(const float *in, float *out, const float *matrix) {
    for (int i = 0; i < 3; i++) {
        float res = 0;
        for (int j = 0; j < 3; j++) {
            res += matrix[3 * i + j] * in[j];
        }
        out[i] = res;
    }
}

float saturate(float x) {
    return min(1, max(x, 0));
}

typedef struct ThreadData {
    float *pixels;
    unsigned short *converted;
    unsigned int width;
    int start;
    int stop;
    float maxMaxComp;
    double sumOfMaxComp;
} ThreadData;

DWORD WINAPI ThreadFunc(LPVOID lpParam) {
    ThreadData *d = (ThreadData *) lpParam;
    float *pixels = d->pixels;
    unsigned short *converted = d->converted;
    unsigned int width = d->width;
    int start = d->start;
    int stop = d->stop;

    float maxMaxComp = 0;
    double sumOfMaxComp = 0;

    for (UINT i = start; i < stop; i++) {
        for (int j = 0; j < width; j++) {
            float *cur = ((float *) pixels + i * 3 * width) + 3 * j;
            float bt2020[3];

            matrixVectorMult(cur, bt2020, (float *) scrgb_to_bt2100);

            for (int k = 0; k < 3; k++) {
                bt2020[k] = saturate(bt2020[k]);
            }

            float maxComp = max(bt2020[0], max(bt2020[1], bt2020[2]));

            if (maxComp > maxMaxComp) {
                maxMaxComp = maxComp;
            }

            sumOfMaxComp += maxComp;

            for (int k = 0; k < 3; k++) {
                converted[3 * width * i + 3 * j + k] = (unsigned short) roundf(
                        pq_inv_eotf(bt2020[k]) * ((1 << INTERMEDIATE_BITS) - 1));
            }
        }
    }

    d->maxMaxComp = maxMaxComp;
    d->sumOfMaxComp = sumOfMaxComp;

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "jxr_to_avif input.jxr [output.avif]\n");
        return 1;
    }

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

        inputFile = szArglist[1];

        if (argc == 3) {
            outputFile = szArglist[2];
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

    if (!IsEqualGUID((void *) &pixelFormat, (void *) &GUID_WICPixelFormat64bppRGBAHalf)) {
        fprintf(stderr, "Wrong pixel format\n");
        return 1;
    }

    IWICBitmapSource *pConverter = NULL;

    hr = WICConvertBitmapSource(&GUID_WICPixelFormat96bppRGBFloat, pBitmapSource, &pConverter);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to convert pixel format\n");
        return 1;
    }

    pBitmapSource->lpVtbl->Release(pBitmapSource);
    pBitmapSource = pConverter;

    unsigned int width, height;

    hr = pBitmapSource->lpVtbl->GetSize(pBitmapSource, &width, &height);

    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get size\n");
        return 1;
    }

    unsigned short *converted = malloc(sizeof(uint16_t) * width * height * 3);

    if (converted == NULL) {
        fprintf(stderr, "Failed to allocate converted pixels\n");
        return 1;
    }

    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    int numThreads = (int) systemInfo.dwNumberOfProcessors;
    printf("Using %d threads\n", numThreads);

    puts("Converting pixels to BT.2100 PQ...");

    uint16_t maxCLL, maxPALL;

    {

        UINT cbStride = width * sizeof(float) * 3;
        UINT cbBufferSize = cbStride * height;

        float *pixels = malloc(cbBufferSize);

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
                                               (BYTE *) pixels);

        if (FAILED(hr)) {
            fprintf(stderr, "Failed to copy pixels\n");
            exit(1);
        }

        HANDLE hThreadArray[numThreads];
        ThreadData *threadData[numThreads];
        DWORD dwThreadIdArray[numThreads];

        int chunkSize = (int) height / numThreads;

        for (int i = 0; i < numThreads; i++) {
            threadData[i] = malloc(sizeof(threadData));
            threadData[i]->pixels = pixels;
            threadData[i]->converted = converted;
            threadData[i]->width = width;
            threadData[i]->start = i * chunkSize;
            if (i != numThreads - 1) {
                threadData[i]->stop = (i + 1) * chunkSize;
            } else {
                threadData[i]->stop = (int) height;
            }

            hThreadArray[i] = CreateThread(
                    NULL,                   // default security attributes
                    0,                      // use default stack size
                    ThreadFunc,       // thread function name
                    threadData[i],          // argument to thread function
                    0,                      // use default creation flags
                    &dwThreadIdArray[i]);   // returns the thread identifier
        }

        WaitForMultipleObjects(numThreads, hThreadArray, TRUE, INFINITE);

        float maxMaxComp = 0;
        double sumOfMaxComp = 0;

        for (int i = 0; i < numThreads; i++) {
            CloseHandle(hThreadArray[i]);

            float tMaxMaxComp = threadData[i]->maxMaxComp;
            if (tMaxMaxComp > maxMaxComp) {
                maxMaxComp = tMaxMaxComp;
            }

            sumOfMaxComp += threadData[i]->sumOfMaxComp;

            free(threadData[i]);
        }

        maxCLL = (uint16_t) roundf(maxMaxComp * 10000);
        maxPALL = (uint16_t) round(10000 * (sumOfMaxComp / (double)((uint64_t) width * height)));

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

    printf("Computed HDR metadata: %u maxCLL, %u maxPALL\n", maxCLL, maxPALL);

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
    encoder->speed = ENCODER_SPEED;
    encoder->maxThreads = numThreads;
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
