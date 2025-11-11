/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * H.264 Bitmap Compression - OPTIMIZED VERSION
 *
 * Based on original code by:
 *   Mike McDonald, Vic Lee, Armin Novak
 *
 * Optimized for low-latency RDP desktop streaming.
 * - No re-initialization per frame
 * - Pre-allocated buffers
 * - Dirty flags for parameter changes
 * - DecodeFrameNoDelay for minimal delay
 * - AVX2-ready compilation assumed
 * - Debug logs disabled in release
 *
 * Copyright 2025 FreeRDP Contributors
 * Licensed under the Apache License, Version 2.0
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <freerdp/log.h>
#include <freerdp/codec/h264.h>
#include <winpr/library.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "wels/codec_def.h"
#include "wels/codec_api.h"
#include "wels/codec_ver.h"

// --- Configuration ---
#define MAX_FRAME_SIZE (1920 * 1080 * 3 / 2)  // Max 1080p I-frame size
#define MAX_THREADS 4                         // Max encoder threads
#define MIN_OPENH264_VERSION_MAJOR 1
#define MIN_OPENH264_VERSION_MINOR 6

// --- Logging ---
#ifdef NDEBUG
#define H264_LOG(level, ...) do {} while(0)
#else
#define H264_LOG(level, ...) WLog_Print(h264->log, level, __VA_ARGS__)
#endif

// --- Type Definitions ---
typedef void (*pWelsGetCodecVersionEx)(OpenH264Version* pVersion);
typedef long (*pWelsCreateDecoder)(ISVCDecoder** ppDecoder);
typedef void (*pWelsDestroyDecoder)(ISVCDecoder* pDecoder);
typedef int (*pWelsCreateSVCEncoder)(ISVCEncoder** ppEncoder);
typedef void (*pWelsDestroySVCEncoder)(ISVCEncoder* pEncoder);

typedef struct {
    BOOL bitrate_changed;
    BOOL framerate_changed;
    BOOL qp_changed;
    BOOL resolution_changed;
} H264_ENC_PARAMS_DIRTY;

typedef struct _H264_CONTEXT_OPENH264 {
#if defined(WITH_OPENH264_LOADING)
    HMODULE lib;
    OpenH264Version version;
#endif
    pWelsGetCodecVersionEx WelsGetCodecVersionEx;
    pWelsCreateDecoder WelsCreateDecoder;
    pWelsDestroyDecoder WelsDestroyDecoder;
    pWelsCreateSVCEncoder WelsCreateSVCEncoder;
    pWelsDestroySVCEncoder WelsDestroySVCEncoder;

    ISVCDecoder* pDecoder;
    ISVCEncoder* pEncoder;

    // Encoder parameters (cached)
    SEncParamExt EncParamExt;
    H264_ENC_PARAMS_DIRTY dirty;

    // Pre-allocated buffers (aligned)
    BYTE* pOutputBuffer;     // Compressed output buffer
    BYTE* pAlignedYUVBuffer; // Aligned YUV input buffer (for stride padding)
    UINT32 allocatedWidth;
    UINT32 allocatedHeight;

} H264_CONTEXT_OPENH264;

#if defined(WITH_OPENH264_LOADING)
static const char* openh264_library_names[] = {
#if defined(_WIN32)
    "openh264.dll",
#elif defined(__APPLE__)
    "libopenh264.dylib",
#else
    "libopenh264.so",
#endif
};
#endif

// --- Trace Callback ---
static void openh264_trace_callback(H264_CONTEXT* h264, int level, const char* message)
{
    if (h264)
        WLog_Print(h264->log, WLOG_TRACE, "%d - %s", level, message);
}

// --- Helper: Align to 16-byte boundary ---
static inline UINT32 align16(UINT32 x) {
    return (x + 15) & ~15;
}

// --- Helper: Load dynamic library ---
#if defined(WITH_OPENH264_LOADING)
static BOOL openh264_load_functionpointers(H264_CONTEXT* h264, const char* name)
{
    H264_CONTEXT_OPENH264* sys = (H264_CONTEXT_OPENH264*)h264->pSystemData;
    if (!sys) return FALSE;

    sys->lib = LoadLibraryA(name);
    if (!sys->lib) return FALSE;

    sys->WelsGetCodecVersionEx = (pWelsGetCodecVersionEx)GetProcAddress(sys->lib, "WelsGetCodecVersionEx");
    sys->WelsCreateDecoder = (pWelsCreateDecoder)GetProcAddress(sys->lib, "WelsCreateDecoder");
    sys->WelsDestroyDecoder = (pWelsDestroyDecoder)GetProcAddress(sys->lib, "WelsDestroyDecoder");
    sys->WelsCreateSVCEncoder = (pWelsCreateSVCEncoder)GetProcAddress(sys->lib, "WelsCreateSVCEncoder");
    sys->WelsDestroySVCEncoder = (pWelsDestroySVCEncoder)GetProcAddress(sys->lib, "WelsDestroySVCEncoder");

    if (!sys->WelsCreateDecoder || !sys->WelsDestroyDecoder ||
        !sys->WelsCreateSVCEncoder || !sys->WelsDestroySVCEncoder ||
        !sys->WelsGetCodecVersionEx)
    {
        FreeLibrary(sys->lib);
        sys->lib = NULL;
        return FALSE;
    }

    sys->WelsGetCodecVersionEx(&sys->version);
    H264_LOG(WLOG_INFO, "Loaded %s v%d.%d.%d", name,
             sys->version.uMajor, sys->version.uMinor, sys->version.uRevision);

    if (sys->version.uMajor < MIN_OPENH264_VERSION_MAJOR ||
        (sys->version.uMajor == MIN_OPENH264_VERSION_MAJOR && sys->version.uMinor < MIN_OPENH264_VERSION_MINOR))
    {
        H264_LOG(WLOG_ERROR, "OpenH264 %s %d.%d.%d is too old, need at least %d.%d.0",
                 name, sys->version.uMajor, sys->version.uMinor, sys->version.uRevision,
                 MIN_OPENH264_VERSION_MAJOR, MIN_OPENH264_VERSION_MINOR);
        FreeLibrary(sys->lib);
        sys->lib = NULL;
        return FALSE;
    }

    return TRUE;
}
#endif

// --- Init: Allocate and configure everything once ---
static BOOL openh264_init(H264_CONTEXT* h264)
{
#if defined(WITH_OPENH264_LOADING)
    size_t i;
    BOOL success = FALSE;
#endif
    UINT32 x;
    long status;
    SDecodingParam sDecParam;
    H264_CONTEXT_OPENH264* sysContexts = NULL;

    // Allocate system data
    h264->numSystemData = 1;
    sysContexts = (H264_CONTEXT_OPENH264*)calloc(h264->numSystemData, sizeof(H264_CONTEXT_OPENH264));
    if (!sysContexts) goto EXCEPTION;
    h264->pSystemData = sysContexts;

    // Load library or bind static symbols
#if defined(WITH_OPENH264_LOADING)
    for (i = 0; i < ARRAYSIZE(openh264_library_names); i++)
    {
        if (openh264_load_functionpointers(h264, openh264_library_names[i]))
        {
            success = TRUE;
            break;
        }
    }
    if (!success) goto EXCEPTION;
#else
    sysContexts->WelsGetCodecVersionEx = WelsGetCodecVersionEx;
    sysContexts->WelsCreateDecoder = WelsCreateDecoder;
    sysContexts->WelsDestroyDecoder = WelsDestroyDecoder;
    sysContexts->WelsCreateSVCEncoder = WelsCreateSVCEncoder;
    sysContexts->WelsDestroySVCEncoder = WelsDestroySVCEncoder;
#endif

    // Initialize each context (only one)
    H264_CONTEXT_OPENH264* sys = &sysContexts[0];

    // --- Initialize Encoder (if compressor) ---
    if (h264->Compressor)
    {
        status = sys->WelsCreateSVCEncoder(&sys->pEncoder);
        if (!sys->pEncoder)
        {
            H264_LOG(WLOG_ERROR, "Failed to create OpenH264 encoder");
            goto EXCEPTION;
        }

        // Get default parameters
        status = sys->pEncoder->GetDefaultParams(sys->pEncoder, &sys->EncParamExt);
        if (status < 0)
        {
            H264_LOG(WLOG_ERROR, "GetDefaultParams failed: %d", status);
            goto EXCEPTION;
        }

        // --- Configure for RDP Desktop Streaming ---
        sys->EncParamExt.iUsageType = SCREEN_CONTENT_REAL_TIME;
        sys->EncParamExt.iPicWidth = (int)h264->width;
        sys->EncParamExt.iPicHeight = (int)h264->height;
        sys->EncParamExt.fMaxFrameRate = (float)h264->FrameRate;
        sys->EncParamExt.iMaxBitrate = UNSPECIFIED_BIT_RATE;
        sys->EncParamExt.bEnableDenoise = 0;
        sys->EncParamExt.bEnableLongTermReference = 0;
        sys->EncParamExt.bEnableFrameSkip = 1; // ✅ Enable frame skipping under load
        sys->EncParamExt.iSpatialLayerNum = 1;
        sys->EncParamExt.iLoopFilterDisableIdc = 1; // Disable deblocking for speed
        sys->EncParamExt.iNumRefFrame = 0;          // No reference frames needed for desktop
        sys->EncParamExt.iMultipleThreadIdc = min(MAX_THREADS, GetLogicalProcessorCount());
        sys->EncParamExt.bUseCabac = 1;             // ✅ Better compression
        sys->EncParamExt.iMotionEstimationSearchRange = 16; // ✅ Reduce search range

        // Single layer
        sys->EncParamExt.sSpatialLayers[0].fFrameRate = h264->FrameRate;
        sys->EncParamExt.sSpatialLayers[0].iVideoWidth = (int)h264->width;
        sys->EncParamExt.sSpatialLayers[0].iVideoHeight = (int)h264->height;
        sys->EncParamExt.sSpatialLayers[0].iMaxSpatialBitrate = sys->EncParamExt.iMaxBitrate;

        // Slice mode for multi-threading
#if (OPENH264_MAJOR == 1) && (OPENH264_MINOR <= 5)
        sys->EncParamExt.sSpatialLayers[0].sSliceCfg.uiSliceMode = SM_AUTO_SLICE;
#else
        sys->EncParamExt.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_FIXEDSLCNUM_SLICE;
#endif

        // Set rate control mode
        switch (h264->RateControlMode)
        {
            case H264_RATECONTROL_VBR:
                sys->EncParamExt.iRCMode = RC_BITRATE_MODE;
                sys->EncParamExt.iTargetBitrate = (int)h264->BitRate;
                sys->EncParamExt.sSpatialLayers[0].iSpatialBitrate = sys->EncParamExt.iTargetBitrate;
                break;
            case H264_RATECONTROL_CQP:
                sys->EncParamExt.iRCMode = RC_OFF_MODE;
                sys->EncParamExt.sSpatialLayers[0].iDLayerQp = (int)h264->QP;
                break;
            default:
                sys->EncParamExt.iRCMode = RC_BITRATE_MODE;
                sys->EncParamExt.iTargetBitrate = 2000000; // Default 2Mbps
                break;
        }

        // Initialize once and only once
        status = sys->pEncoder->InitializeExt(sys->pEncoder, &sys->EncParamExt);
        if (status < 0)
        {
            H264_LOG(WLOG_ERROR, "Failed to initialize OpenH264 encoder: %d", status);
            goto EXCEPTION;
        }

        // Cache initial params
        sys->dirty.bitrate_changed = FALSE;
        sys->dirty.framerate_changed = FALSE;
        sys->dirty.qp_changed = FALSE;
        sys->dirty.resolution_changed = FALSE;

        H264_LOG(WLOG_INFO, "OpenH264 encoder initialized: %dx%d @%.1f fps, RC=%s",
                 sys->EncParamExt.iPicWidth, sys->EncParamExt.iPicHeight,
                 sys->EncParamExt.fMaxFrameRate,
                 (sys->EncParamExt.iRCMode == RC_BITRATE_MODE) ? "VBR" : "CQP");
    }

        // --- Initialize Decoder ---
    else
    {
        status = sys->WelsCreateDecoder(&sys->pDecoder);
        if (!sys->pDecoder)
        {
            H264_LOG(WLOG_ERROR, "Failed to create OpenH264 decoder");
            goto EXCEPTION;
        }

        ZeroMemory(&sDecParam, sizeof(sDecParam));
#if (OPENH264_MAJOR == 1) && (OPENH264_MINOR <= 5)
        sDecParam.eOutputColorFormat = videoFormatI420;
#endif
        sDecParam.eEcActiveIdc = ERROR_CON_FRAME_COPY;
        sDecParam.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;

        status = sys->pDecoder->Initialize(sys->pDecoder, &sDecParam);
        if (status != 0)
        {
            H264_LOG(WLOG_ERROR, "Failed to initialize OpenH264 decoder: %ld", status);
            goto EXCEPTION;
        }

#if (OPENH264_MAJOR == 1) && (OPENH264_MINOR <= 5)
        EVideoFormatType videoFormat = videoFormatI420;
        status = sys->pDecoder->SetOption(sys->pDecoder, DECODER_OPTION_DATAFORMAT, &videoFormat);
        if (status != 0)
        {
            H264_LOG(WLOG_ERROR, "Failed to set data format: %ld", status);
            goto EXCEPTION;
        }
#endif

        // Enable trace if needed
        if (WLog_GetLogLevel(h264->log) == WLOG_TRACE)
        {
            int traceLevel = WELS_LOG_DEBUG;
            status = sys->pDecoder->SetOption(sys->pDecoder, DECODER_OPTION_TRACE_LEVEL, &traceLevel);
            if (status != 0) H264_LOG(WLOG_WARN, "Failed to set trace level: %ld", status);

            status = sys->pDecoder->SetOption(sys->pDecoder, DECODER_OPTION_TRACE_CALLBACK_CONTEXT, &h264);
            if (status != 0) H264_LOG(WLOG_WARN, "Failed to set trace context: %ld", status);

            WelsTraceCallback traceCallback = (WelsTraceCallback)openh264_trace_callback;
            status = sys->pDecoder->SetOption(sys->pDecoder, DECODER_OPTION_TRACE_CALLBACK, &traceCallback);
            if (status != 0) H264_LOG(WLOG_WARN, "Failed to set trace callback: %ld", status);
        }
    }

    // --- Pre-allocate buffers ---
    sys->allocatedWidth = align16(h264->width);
    sys->allocatedHeight = h264->height;
    sys->pOutputBuffer = (BYTE*)malloc(MAX_FRAME_SIZE);
    sys->pAlignedYUVBuffer = (BYTE*)aligned_alloc(16, sys->allocatedWidth * sys->allocatedHeight * 3 / 2);

    if (!sys->pOutputBuffer || !sys->pAlignedYUVBuffer)
    {
        H264_LOG(WLOG_ERROR, "Failed to allocate pre-buffer memory");
        goto EXCEPTION;
    }

    H264_LOG(WLOG_INFO, "Pre-allocated output buffer: %zu bytes, aligned YUV: %zu bytes",
             MAX_FRAME_SIZE, sys->allocatedWidth * sys->allocatedHeight * 3 / 2);

    return TRUE;

    EXCEPTION:
    openh264_uninit(h264);
    return FALSE;
}

// --- Uninit: Clean up everything ---
static void openh264_uninit(H264_CONTEXT* h264)
{
    if (!h264 || !h264->pSystemData) return;

    H264_CONTEXT_OPENH264* sysContexts = (H264_CONTEXT_OPENH264*)h264->pSystemData;
    H264_CONTEXT_OPENH264* sys = &sysContexts[0];

    // Destroy encoder/decoder
    if (sys->pEncoder)
    {
        sys->pEncoder->Uninitialize(sys->pEncoder);
        sys->WelsDestroySVCEncoder(sys->pEncoder);
        sys->pEncoder = NULL;
    }

    if (sys->pDecoder)
    {
        sys->pDecoder->Uninitialize(sys->pDecoder);
        sys->WelsDestroyDecoder(sys->pDecoder);
        sys->pDecoder = NULL;
    }

    // Free buffers
    free(sys->pOutputBuffer);
    free(sys->pAlignedYUVBuffer);

#if defined(WITH_OPENH264_LOADING)
    if (sys->lib)
    {
        FreeLibrary(sys->lib);
        sys->lib = NULL;
    }
#endif

    free(h264->pSystemData);
    h264->pSystemData = NULL;
}

// --- Decompress: Decode H.264 to YUV ---
static int openh264_decompress(H264_CONTEXT* h264, const BYTE* pSrcData, UINT32 SrcSize)
{
    H264_CONTEXT_OPENH264* sys = (H264_CONTEXT_OPENH264*)h264->pSystemData;
    if (!sys || !sys->pDecoder) return -2001;

    DECODING_STATE state;
    SBufferInfo sBufferInfo = {0};
    SSysMEMBuffer* pSystemBuffer;
    UINT32* iStride = h264->iStride;
    BYTE** pYUVData = h264->pYUVData;

    // Clear output
    pYUVData[0] = pYUVData[1] = pYUVData[2] = NULL;
    ZeroMemory(&sBufferInfo, sizeof(sBufferInfo));

    // ✅ Use DecodeFrameNoDelay() for minimal latency (available in 1.7+)
#if OPENH264_MAJOR >= 2
    state = sys->pDecoder->DecodeFrameNoDelay(sys->pDecoder, pSrcData, SrcSize, pYUVData, &sBufferInfo);
#else
    state = sys->pDecoder->DecodeFrame2(sys->pDecoder, pSrcData, SrcSize, pYUVData, &sBufferInfo);
#endif

    // Handle missing parameter sets (first frame)
    if (sBufferInfo.iBufferStatus != 1)
    {
        if (state == dsNoParamSets)
        {
            state = sys->pDecoder->DecodeFrame2(sys->pDecoder, NULL, 0, pYUVData, &sBufferInfo);
        }
        else if (state == dsErrorFree)
        {
            state = sys->pDecoder->DecodeFrame2(sys->pDecoder, NULL, 0, pYUVData, &sBufferInfo);
        }
        else
        {
            H264_LOG(WLOG_WARN, "DecodeFrame state: 0x%04X, iBufferStatus: %d", state, sBufferInfo.iBufferStatus);
            return -2002;
        }
    }

    if (state != dsErrorFree)
    {
        H264_LOG(WLOG_WARN, "DecodeFrame state: 0x%02X", state);
        return -2003;
    }

#if OPENH264_MAJOR >= 2
    state = sys->pDecoder->FlushFrame(sys->pDecoder, pYUVData, &sBufferInfo);
    if (state != dsErrorFree)
    {
        H264_LOG(WLOG_WARN, "FlushFrame state: 0x%02X", state);
        return -2003;
    }
#endif

    pSystemBuffer = &sBufferInfo.UsrData.sSystemBuffer;

    // Validate output
    iStride[0] = pSystemBuffer->iStride[0];
    iStride[1] = pSystemBuffer->iStride[1];
    iStride[2] = pSystemBuffer->iStride[1]; // U and V have same stride

    if (sBufferInfo.iBufferStatus != 1)
    {
        H264_LOG(WLOG_WARN, "DecodeFrame iBufferStatus: %d", sBufferInfo.iBufferStatus);
        return 0;
    }

    if (pSystemBuffer->iFormat != videoFormatI420)
    {
        H264_LOG(WLOG_WARN, "Unexpected format: %d (expected I420)", pSystemBuffer->iFormat);
        return -2004;
    }

    if (!pYUVData[0] || !pYUVData[1] || !pYUVData[2])
    {
        H264_LOG(WLOG_WARN, "Null YUV output pointers");
        return -2005;
    }

    return 1;
}

// --- Compress: Encode YUV to H.264 ---
static int openh264_compress(H264_CONTEXT* h264, const BYTE** pYUVData, const UINT32* iStride,
                             BYTE** ppDstData, UINT32* pDstSize)
{
    H264_CONTEXT_OPENH264* sys = (H264_CONTEXT_OPENH264*)h264->pSystemData;
    if (!sys || !sys->pEncoder) return -1;

    // Validate inputs
    if (!pYUVData[0] || !pYUVData[1] || !pYUVData[2]) return -1;
    if (h264->width > INT_MAX || h264->height > INT_MAX) return -1;
    if (h264->FrameRate > INT_MAX || h264->NumberOfThreads > INT_MAX ||
        h264->BitRate > INT_MAX || h264->QP > INT_MAX) return -1;

    // --- Handle resolution change (only if truly changed) ---
    if (sys->EncParamExt.iPicWidth != (int)h264->width ||
        sys->EncParamExt.iPicHeight != (int)h264->height)
    {
        H264_LOG(WLOG_INFO, "Resolution changed: %dx%d -> %dx%d",
                 sys->EncParamExt.iPicWidth, sys->EncParamExt.iPicHeight,
                 h264->width, h264->height);

        sys->EncParamExt.iPicWidth = (int)h264->width;
        sys->EncParamExt.iPicHeight = (int)h264->height;
        sys->EncParamExt.sSpatialLayers[0].iVideoWidth = (int)h264->width;
        sys->EncParamExt.sSpatialLayers[0].iVideoHeight = (int)h264->height;

        sys->dirty.resolution_changed = TRUE;
    }

    // --- Update parameters only if changed ---
    if (sys->EncParamExt.iRCMode == RC_BITRATE_MODE)
    {
        if (sys->EncParamExt.iTargetBitrate != (int)h264->BitRate)
        {
            SBitrateInfo bitrate = {0};
            bitrate.iLayer = SPATIAL_LAYER_ALL;
            bitrate.iBitrate = (int)h264->BitRate;
            int status = sys->pEncoder->SetOption(sys->pEncoder, ENCODER_OPTION_BITRATE, &bitrate);
            if (status < 0)
            {
                H264_LOG(WLOG_ERROR, "Set bitrate failed: %d", status);
                return status;
            }
            sys->EncParamExt.iTargetBitrate = (int)h264->BitRate;
            sys->dirty.bitrate_changed = FALSE;
        }

        if (sys->EncParamExt.fMaxFrameRate != h264->FrameRate)
        {
            int status = sys->pEncoder->SetOption(sys->pEncoder, ENCODER_OPTION_FRAME_RATE, &h264->FrameRate);
            if (status < 0)
            {
                H264_LOG(WLOG_ERROR, "Set framerate failed: %d", status);
                return status;
            }
            sys->EncParamExt.fMaxFrameRate = h264->FrameRate;
            sys->dirty.framerate_changed = FALSE;
        }
    }
    else if (sys->EncParamExt.iRCMode == RC_OFF_MODE)
    {
        if (sys->EncParamExt.sSpatialLayers[0].iDLayerQp != (int)h264->QP)
        {
            int status = sys->pEncoder->SetOption(sys->pEncoder, ENCODER_OPTION_SVC_ENCODE_PARAM_EXT, &sys->EncParamExt);
            if (status < 0)
            {
                H264_LOG(WLOG_ERROR, "Set QP failed: %d", status);
                return status;
            }
            sys->EncParamExt.sSpatialLayers[0].iDLayerQp = (int)h264->QP;
            sys->dirty.qp_changed = FALSE;
        }
    }

    // --- Reinitialize encoder ONLY if resolution changed ---
    if (sys->dirty.resolution_changed)
    {
        int status = sys->pEncoder->InitializeExt(sys->pEncoder, &sys->EncParamExt);
        if (status < 0)
        {
            H264_LOG(WLOG_ERROR, "Re-initialize encoder failed: %d", status);
            return status;
        }
        sys->dirty.resolution_changed = FALSE;
    }

    // --- Prepare input YUV (align stride if needed) ---
    SSourcePicture pic = {0};
    pic.iPicWidth = (int)h264->width;
    pic.iPicHeight = (int)h264->height;
    pic.iColorFormat = videoFormatI420;
    pic.iStride[0] = (int)iStride[0];
    pic.iStride[1] = (int)iStride[1];
    pic.iStride[2] = (int)iStride[2];
    pic.pData[0] = (unsigned char*)pYUVData[0];
    pic.pData[1] = (unsigned char*)pYUVData[1];
    pic.pData[2] = (unsigned char*)pYUVData[2];

    // If stride is not aligned to 16, copy to pre-allocated buffer
    if (iStride[0] != align16(h264->width) || iStride[1] != align16(h264->width / 2))
    {
        UINT32 ySize = align16(h264->width) * h264->height;
        UINT32 uvSize = align16(h264->width / 2) * (h264->height / 2);

        // Copy Y
        for (int y = 0; y < h264->height; y++)
            memcpy(sys->pAlignedYUVBuffer + y * align16(h264->width),
                   pYUVData[0] + y * iStride[0],
                   h264->width);

        // Copy U
        for (int y = 0; y < h264->height / 2; y++)
            memcpy(sys->pAlignedYUVBuffer + ySize + y * align16(h264->width / 2),
                   pYUVData[1] + y * iStride[1],
                   h264->width / 2);

        // Copy V
        for (int y = 0; y < h264->height / 2; y++)
            memcpy(sys->pAlignedYUVBuffer + ySize + uvSize + y * align16(h264->width / 2),
                   pYUVData[2] + y * iStride[2],
                   h264->width / 2);

        pic.pData[0] = sys->pAlignedYUVBuffer;
        pic.pData[1] = sys->pAlignedYUVBuffer + ySize;
        pic.pData[2] = sys->pAlignedYUVBuffer + ySize + uvSize;
        pic.iStride[0] = align16(h264->width);
        pic.iStride[1] = align16(h264->width / 2);
        pic.iStride[2] = align16(h264->width / 2);
    }

    // --- Encode ---
    SFrameBSInfo info = {0};
    int status = sys->pEncoder->EncodeFrame(sys->pEncoder, &pic, &info);
    if (status < 0)
    {
        H264_LOG(WLOG_ERROR, "EncodeFrame failed: %d", status);
        return status;
    }

    // --- Copy output ---
    *ppDstData = sys->pOutputBuffer;
    *pDstSize = 0;

    for (int i = 0; i < info.iLayerNum; i++)
    {
        for (int j = 0; j < info.sLayerInfo[i].iNalCount; j++)
        {
            UINT32 nalLen = info.sLayerInfo[i].pNalLengthInByte[j];
            if (*pDstSize + nalLen > MAX_FRAME_SIZE)
            {
                H264_LOG(WLOG_WARN, "Output buffer overflow! Truncating.");
                break;
            }
            memcpy(sys->pOutputBuffer + *pDstSize,
                   info.sLayerInfo[i].pBsBuf + info.sLayerInfo[i].pOffsetInByte[j],
                   nalLen);
            *pDstSize += nalLen;
        }
    }

    return 1;
}

// --- Export subsystem ---
H264_CONTEXT_SUBSYSTEM g_Subsystem_OpenH264 = {
        "OpenH264_Optimized",  // Name
        openh264_init,
        openh264_uninit,
        openh264_decompress,
        openh264_compress
};