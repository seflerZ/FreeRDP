/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * H.264 Bitmap Compression
 *
 * Copyright 2022 Ely Ronnen <elyronnen@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <winpr/wlog.h>
#include <winpr/assert.h>
#include <winpr/library.h>
#include <winpr/winpr.h> // for WINPR_MIN

#include <freerdp/log.h>
#include <freerdp/codec/h264.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include "h264.h"

#define RESOLVE_MEDIANDK_FUNC(sys, name)                                          \
	({                                                                            \
		BOOL rc = TRUE;                                                           \
		sys->fn##name = GetProcAddress(sys->mediandkLibrary, #name);              \
		if (sys->fn##name == NULL)                                                \
		{                                                                         \
			WLog_Print(h264->log, WLOG_ERROR,                                     \
			           "Error resolving function " #name " from libmediandk.so"); \
			rc = FALSE;                                                           \
		}                                                                         \
		rc;                                                                       \
	})

#define RESOLVE_MEDIANDK_VARIABLE(sys, member, exported)                             \
	({                                                                               \
		BOOL rc = FALSE;                                                             \
		const char** temp = GetProcAddress(sys->mediandkLibrary, exported);          \
		if (temp == NULL)                                                            \
		{                                                                            \
			WLog_Print(h264->log, WLOG_ERROR,                                        \
			           "Error resolving variable " exported " from libmediandk.so"); \
		}                                                                            \
		else                                                                         \
		{                                                                            \
			sys->member = *temp;                                                     \
			rc = TRUE;                                                               \
		}                                                                            \
		rc;                                                                          \
	})

// ✅ 正确声明 AMediaFormat_getInt32 函数指针类型（返回 BOOL，参数为指针）
typedef BOOL (*AMediaFormat_getInt32_t)(AMediaFormat*, const char*, int32_t*);

typedef AMediaFormat* (*AMediaFormat_new_t)(void);
typedef media_status_t (*AMediaFormat_delete_t)(AMediaFormat*);
typedef const char* (*AMediaFormat_toString_t)(AMediaFormat*);
typedef void (*AMediaFormat_setInt32_t)(AMediaFormat*, const char*, int32_t);
typedef void (*AMediaFormat_setString_t)(AMediaFormat*, const char*, const char*);
typedef AMediaCodec* (*AMediaCodec_createDecoderByType_t)(const char*);
typedef media_status_t (*AMediaCodec_delete_t)(AMediaCodec*);
typedef media_status_t (*AMediaCodec_configure_t)(AMediaCodec*, const AMediaFormat*, ANativeWindow*,
                                                  AMediaCrypto*, uint32_t);
typedef media_status_t (*AMediaCodec_start_t)(AMediaCodec*);
typedef media_status_t (*AMediaCodec_stop_t)(AMediaCodec*);
typedef uint8_t* (*AMediaCodec_getInputBuffer_t)(AMediaCodec*, size_t, size_t*);
typedef uint8_t* (*AMediaCodec_getOutputBuffer_t)(AMediaCodec*, size_t, size_t*);
typedef ssize_t (*AMediaCodec_dequeueInputBuffer_t)(AMediaCodec*, int64_t);
typedef media_status_t (*AMediaCodec_queueInputBuffer_t)(AMediaCodec*, size_t, ssize_t, size_t,
                                                         uint64_t, uint32_t);
typedef ssize_t (*AMediaCodec_dequeueOutputBuffer_t)(AMediaCodec*, AMediaCodecBufferInfo*, int64_t);
typedef AMediaFormat* (*AMediaCodec_getOutputFormat_t)(AMediaCodec*);
typedef media_status_t (*AMediaCodec_releaseOutputBuffer_t)(AMediaCodec*, size_t, bool);
typedef media_status_t (*AMediaCodec_setParameters_t)(AMediaCodec*, const AMediaFormat*); // 26
typedef media_status_t (*AMediaCodec_getName_t)(AMediaCodec*, char**);                    // 28
typedef void (*AMediaCodec_releaseName_t)(AMediaCodec*, char*);                           // 28
typedef AMediaFormat* (*AMediaCodec_getInputFormat_t)(AMediaCodec*);                      // 28

const int COLOR_FormatYUV420Planar = 19;
const int COLOR_FormatYUV420Flexible = 0x7f420888;

struct _H264_CONTEXT_MEDIACODEC
{
    AMediaCodec* decoder;
    AMediaFormat* inputFormat;   // ✅ 必须有！
    AMediaFormat* outputFormat;  // ✅ 必须有！
    int32_t width;
    int32_t height;
    int32_t lastWidth;
    int32_t lastHeight;
    ssize_t currentOutputBufferIndex;
    BOOL formatChanged;

    // libmediandk.so imports
    HMODULE mediandkLibrary;

    AMediaFormat_new_t fnAMediaFormat_new;
    AMediaFormat_delete_t fnAMediaFormat_delete;
    AMediaFormat_toString_t fnAMediaFormat_toString;
    AMediaFormat_setInt32_t fnAMediaFormat_setInt32;
    AMediaFormat_getInt32_t fnAMediaFormat_getInt32; // ✅ 正确声明：返回 BOOL，参数是 int32_t*
    AMediaFormat_setString_t fnAMediaFormat_setString;
    AMediaCodec_createDecoderByType_t fnAMediaCodec_createDecoderByType;
    AMediaCodec_delete_t fnAMediaCodec_delete;
    AMediaCodec_configure_t fnAMediaCodec_configure;
    AMediaCodec_start_t fnAMediaCodec_start;
    AMediaCodec_stop_t fnAMediaCodec_stop;
    AMediaCodec_getInputBuffer_t fnAMediaCodec_getInputBuffer;
    AMediaCodec_getOutputBuffer_t fnAMediaCodec_getOutputBuffer;
    AMediaCodec_dequeueInputBuffer_t fnAMediaCodec_dequeueInputBuffer;
    AMediaCodec_queueInputBuffer_t fnAMediaCodec_queueInputBuffer;
    AMediaCodec_dequeueOutputBuffer_t fnAMediaCodec_dequeueOutputBuffer;
    AMediaCodec_getOutputFormat_t fnAMediaCodec_getOutputFormat;
    AMediaCodec_releaseOutputBuffer_t fnAMediaCodec_releaseOutputBuffer;
    AMediaCodec_setParameters_t fnAMediaCodec_setParameters;
    AMediaCodec_getName_t fnAMediaCodec_getName;
    AMediaCodec_releaseName_t fnAMediaCodec_releaseName;
    AMediaCodec_getInputFormat_t fnAMediaCodec_getInputFormat;

    const char* gAMediaFormatKeyMime;
    const char* gAMediaFormatKeyWidth;
    const char* gAMediaFormatKeyHeight;
    const char* gAMediaFormatKeyFrameRate;
    const char* gAMediaFormatKeyBitRate;
    const char* gAMediaFormatKeyColorFormat;

    // 缓存输出缓冲区
    size_t lastOutputBufferSize;
    uint8_t* cachedOutputBuffer;
    size_t cachedOutputBufferSize;
};

typedef struct _H264_CONTEXT_MEDIACODEC H264_CONTEXT_MEDIACODEC;

static int load_libmediandk(H264_CONTEXT* h264)
{
    BOOL rc;
    H264_CONTEXT_MEDIACODEC* sys;

    WINPR_ASSERT(h264);

    sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
    WINPR_ASSERT(sys);

    WLog_Print(h264->log, WLOG_DEBUG, "MediaCodec Loading libmediandk.so");

    sys->mediandkLibrary = LoadLibraryA("libmediandk.so");
    if (sys->mediandkLibrary == NULL)
    {
        WLog_Print(h264->log, WLOG_WARN, "Error loading libmediandk.so");
        return -1;
    }

    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaFormat_new);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaFormat_delete);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaFormat_toString);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaFormat_setInt32);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaFormat_getInt32); // ✅ 必须加载！
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaFormat_setString);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_createDecoderByType);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_delete);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_configure);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_start);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_stop);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_getInputBuffer);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_getOutputBuffer);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_dequeueInputBuffer);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_queueInputBuffer);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_dequeueOutputBuffer);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_getOutputFormat);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_releaseOutputBuffer);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_setParameters);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_getName);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_releaseName);
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaCodec_getInputFormat);
    if (!rc) return -1;

    rc = RESOLVE_MEDIANDK_VARIABLE(sys, gAMediaFormatKeyMime, "AMEDIAFORMAT_KEY_MIME");
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_VARIABLE(sys, gAMediaFormatKeyWidth, "AMEDIAFORMAT_KEY_WIDTH");
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_VARIABLE(sys, gAMediaFormatKeyFrameRate, "AMEDIAFORMAT_KEY_FRAME_RATE");
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_VARIABLE(sys, gAMediaFormatKeyBitRate, "AMEDIAFORMAT_KEY_BIT_RATE");
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_VARIABLE(sys, gAMediaFormatKeyHeight, "AMEDIAFORMAT_KEY_HEIGHT");
    if (!rc) return -1;
    rc = RESOLVE_MEDIANDK_VARIABLE(sys, gAMediaFormatKeyColorFormat, "AMEDIAFORMAT_KEY_COLOR_FORMAT");
    if (!rc) return -1;

    return 0;
}

static void unload_libmediandk(H264_CONTEXT* h264)
{
    H264_CONTEXT_MEDIACODEC* sys;

    WINPR_ASSERT(h264);

    sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
    WINPR_ASSERT(sys);

    if (NULL == sys->mediandkLibrary)
    {
        return;
    }

    FreeLibrary(sys->mediandkLibrary);
}

static void set_mediacodec_format(H264_CONTEXT* h264, AMediaFormat** formatVariable,
                                  AMediaFormat* newFormat)
{
    media_status_t status = AMEDIA_OK;
    H264_CONTEXT_MEDIACODEC* sys;

    WINPR_ASSERT(h264);
    WINPR_ASSERT(formatVariable);

    sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
    WINPR_ASSERT(sys);

    if (*formatVariable != NULL)
    {
        status = sys->fnAMediaFormat_delete(*formatVariable);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "Error AMediaFormat_delete %d", status);
        }
    }

    *formatVariable = newFormat;
}

static void release_current_outputbuffer(H264_CONTEXT* h264)
{
    media_status_t status = AMEDIA_OK;
    H264_CONTEXT_MEDIACODEC* sys;

    WINPR_ASSERT(h264);
    sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
    WINPR_ASSERT(sys);

    if (sys->currentOutputBufferIndex < 0)
    {
        return;
    }

    status =
            sys->fnAMediaCodec_releaseOutputBuffer(sys->decoder, sys->currentOutputBufferIndex, FALSE);
    if (status != AMEDIA_OK)
    {
        WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_releaseOutputBuffer %d", status);
    }

    sys->currentOutputBufferIndex = -1;
}

static int mediacodec_compress(H264_CONTEXT* h264, const BYTE** pSrcYuv, const UINT32* pStride,
                               BYTE** ppDstData, UINT32* pDstSize)
{
    WINPR_ASSERT(h264);
    WINPR_ASSERT(pSrcYuv);
    WINPR_ASSERT(pStride);
    WINPR_ASSERT(ppDstData);
    WINPR_ASSERT(pDstSize);

    WLog_Print(h264->log, WLOG_ERROR, "MediaCodec is not supported as an encoder");
    return -1;
}

static int mediacodec_decompress(H264_CONTEXT* h264, const BYTE* pSrcData, UINT32 SrcSize)
{
    ssize_t inputBufferId;
    size_t inputBufferSize;
    uint8_t* inputBuffer;
    media_status_t status;
    H264_CONTEXT_MEDIACODEC* sys;
    AMediaCodecBufferInfo bufferInfo;
    ssize_t outputBufferId;
    uint8_t* outputBuffer;
    size_t outputBufferSize;
    BYTE** pYUVData;
    UINT32* iStride;

    WINPR_ASSERT(h264);
    WINPR_ASSERT(pSrcData);

    sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
    WINPR_ASSERT(sys);

    pYUVData = h264->pYUVData;
    iStride = h264->iStride;

    release_current_outputbuffer(h264);

    // ✅ 关键：在每次解码前，清空目标 YUV 缓冲区（避免重影）
    // FreeRDP 的 pYUVData 指向的是“目标显示缓冲区”，我们需要清空它
    if (sys->decoder && pYUVData[0] && sys->width > 0 && sys->height > 0)
    {
        // 清空 Y 分量（全黑）
        memset(pYUVData[0], 0, iStride[0] * sys->height);

        // 清空 U 分量
        memset(pYUVData[1], 128, iStride[1] * ((sys->height + 1) / 2));

        // 清空 V 分量
        memset(pYUVData[2], 128, iStride[2] * ((sys->height + 1) / 2));
    }

    // ✅ ✅ ✅ 关键：如果解码器未创建，且宽高有效 → 创建解码器
    if (!sys->decoder && h264->width > 0 && h264->height > 0)
    {
        WLog_Print(h264->log, WLOG_INFO, "MediaCodec: Initializing decoder with resolution %dx%d",
                   h264->width, h264->height);

        sys->decoder = sys->fnAMediaCodec_createDecoderByType("video/avc");
        if (!sys->decoder)
        {
            WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_createDecoderByType failed");
            return -1;
        }

        sys->inputFormat = sys->fnAMediaFormat_new();
        if (!sys->inputFormat)
        {
            WLog_Print(h264->log, WLOG_ERROR, "AMediaFormat_new failed");
            goto EXCEPTION;
        }

        sys->fnAMediaFormat_setString(sys->inputFormat, sys->gAMediaFormatKeyMime, "video/avc");
        sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyWidth, h264->width);
        sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyHeight, h264->height);
        sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyFrameRate, h264->FrameRate);
        sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyBitRate, h264->BitRate);
        sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyColorFormat,
                                     COLOR_FormatYUV420Planar); // ✅ 使用标准值

        // ✅ 配置解码器
        status = sys->fnAMediaCodec_configure(sys->decoder, sys->inputFormat, NULL, NULL, 0);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_configure failed: %d", status);
            goto EXCEPTION;
        }

        // ✅ 获取实际配置的格式（调试用）
        sys->inputFormat = sys->fnAMediaCodec_getInputFormat(sys->decoder);
        sys->outputFormat = sys->fnAMediaCodec_getOutputFormat(sys->decoder);

        status = sys->fnAMediaCodec_start(sys->decoder);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_start failed: %d", status);
            goto EXCEPTION;
        }

        sys->width = h264->width;
        sys->height = h264->height;
        sys->lastWidth = sys->width;
        sys->lastHeight = sys->height;

        WLog_Print(h264->log, WLOG_INFO, "MediaCodec decoder initialized successfully");
    }

    // ✅ 如果解码器仍未创建，说明宽高仍为0，跳过处理
    if (!sys->decoder)
    {
        WLog_Print(h264->log, WLOG_WARN, "MediaCodec: Waiting for valid width/height (current: %dx%d)",
                   h264->width, h264->height);
        return 0; // 不处理，等待下一帧
    }

    // ✅ 正常解码流程
    UINT32 offset = 0;
    int retry = 0;

    while (offset < SrcSize && retry < 3)
    {
        inputBufferId = sys->fnAMediaCodec_dequeueInputBuffer(sys->decoder, 10000); // 10ms
        if (inputBufferId < 0)
        {
            if (inputBufferId == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
            {
                retry++;
                continue;
            }
            else
            {
                WLog_Print(h264->log, WLOG_ERROR, "dequeueInputBuffer failed: %zd", inputBufferId);
                return -1;
            }
        }

        inputBuffer = sys->fnAMediaCodec_getInputBuffer(sys->decoder, inputBufferId, &inputBufferSize);
        if (!inputBuffer)
        {
            WLog_Print(h264->log, WLOG_ERROR, "getInputBuffer failed");
            return -1;
        }

        UINT32 copySize = (SrcSize - offset) < inputBufferSize ? (SrcSize - offset) : inputBufferSize;
        memcpy(inputBuffer, pSrcData + offset, copySize);

        status = sys->fnAMediaCodec_queueInputBuffer(sys->decoder, inputBufferId, 0, copySize, 0, 0);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "queueInputBuffer failed: %d", status);
            return -1;
        }

        offset += copySize;
        retry = 0;
    }

    if (offset < SrcSize)
    {
        WLog_Print(h264->log, WLOG_WARN, "Failed to enqueue all data (%u/%u)", offset, SrcSize);
    }

    outputBufferId = sys->fnAMediaCodec_dequeueOutputBuffer(sys->decoder, &bufferInfo, 5000); // 5ms
    if (outputBufferId == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
    {
        return 0;
    }
    else if (outputBufferId == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
    {
        AMediaFormat* newFormat = sys->fnAMediaCodec_getOutputFormat(sys->decoder);
        if (!newFormat)
        {
            WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_getOutputFormat failed after format change");
            return -1;
        }

        int32_t newWidth = 0, newHeight = 0;
        if (!sys->fnAMediaFormat_getInt32(newFormat, sys->gAMediaFormatKeyWidth, &newWidth))
        {
            WLog_Print(h264->log, WLOG_ERROR, "Failed to get width from output format");
            sys->fnAMediaFormat_delete(newFormat);
            return -1;
        }
        if (!sys->fnAMediaFormat_getInt32(newFormat, sys->gAMediaFormatKeyHeight, &newHeight))
        {
            WLog_Print(h264->log, WLOG_ERROR, "Failed to get height from output format");
            sys->fnAMediaFormat_delete(newFormat);
            return -1;
        }

        if (newWidth != sys->width || newHeight != sys->height)
        {
            sys->width = newWidth;
            sys->height = newHeight;
            WLog_Print(h264->log, WLOG_INFO, "Resolution changed to %dx%d", sys->width, sys->height);
        }

        if (sys->outputFormat)
        {
            sys->fnAMediaFormat_delete(sys->outputFormat);
            sys->outputFormat = NULL;
        }
        sys->outputFormat = newFormat;

        outputBufferId = sys->fnAMediaCodec_dequeueOutputBuffer(sys->decoder, &bufferInfo, 5000);
        if (outputBufferId == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        {
            return 0;
        }
        else if (outputBufferId < 0)
        {
            WLog_Print(h264->log, WLOG_ERROR, "dequeueOutputBuffer after format change failed: %zd", outputBufferId);
            return -1;
        }
    }
    else if (outputBufferId < 0)
    {
        WLog_Print(h264->log, WLOG_ERROR, "dequeueOutputBuffer returned unknown value: %zd", outputBufferId);
        return -1;
    }

    outputBuffer = sys->fnAMediaCodec_getOutputBuffer(sys->decoder, outputBufferId, &outputBufferSize);
    if (!outputBuffer)
    {
        WLog_Print(h264->log, WLOG_ERROR, "getOutputBuffer failed");
        sys->fnAMediaCodec_releaseOutputBuffer(sys->decoder, outputBufferId, false);
        return -1;
    }

    if (sys->cachedOutputBufferSize < outputBufferSize)
    {
        if (sys->cachedOutputBuffer)
            free(sys->cachedOutputBuffer);
        sys->cachedOutputBuffer = malloc(outputBufferSize);
        if (!sys->cachedOutputBuffer)
        {
            WLog_Print(h264->log, WLOG_ERROR, "Failed to allocate output buffer");
            sys->fnAMediaCodec_releaseOutputBuffer(sys->decoder, outputBufferId, false);
            return -1;
        }
        sys->cachedOutputBufferSize = outputBufferSize;
    }

    memcpy(sys->cachedOutputBuffer, outputBuffer, outputBufferSize);

    iStride[0] = sys->width;
    iStride[1] = (sys->width + 1) / 2;
    iStride[2] = (sys->width + 1) / 2;

    pYUVData[0] = sys->cachedOutputBuffer;
    pYUVData[1] = sys->cachedOutputBuffer + (iStride[0] * sys->height);
    pYUVData[2] = pYUVData[1] + (iStride[1] * ((sys->height + 1) / 2));

    sys->currentOutputBufferIndex = outputBufferId;

    return 1;

    EXCEPTION:
    if (sys->decoder)
    {
        sys->fnAMediaCodec_stop(sys->decoder);
        sys->fnAMediaCodec_delete(sys->decoder);
        sys->decoder = NULL;
    }
    if (sys->inputFormat)
    {
        sys->fnAMediaFormat_delete(sys->inputFormat);
        sys->inputFormat = NULL;
    }
    if (sys->outputFormat)
    {
        sys->fnAMediaFormat_delete(sys->outputFormat);
        sys->outputFormat = NULL;
    }
    return -1;
}

static void mediacodec_uninit(H264_CONTEXT* h264)
{
    media_status_t status = AMEDIA_OK;
    H264_CONTEXT_MEDIACODEC* sys;

    WINPR_ASSERT(h264);

    sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;

    WLog_Print(h264->log, WLOG_DEBUG, "Uninitializing MediaCodec");

    if (!sys)
        return;

    if (sys->decoder)
    {
        release_current_outputbuffer(h264);
        status = sys->fnAMediaCodec_stop(sys->decoder);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_stop %d", status);
        }

        status = sys->fnAMediaCodec_delete(sys->decoder);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_delete %d", status);
        }

        sys->decoder = NULL;
    }

    set_mediacodec_format(h264, &sys->inputFormat, NULL);
    set_mediacodec_format(h264, &sys->outputFormat, NULL);

    if (sys->cachedOutputBuffer)
    {
        free(sys->cachedOutputBuffer);
        sys->cachedOutputBuffer = NULL;
        sys->cachedOutputBufferSize = 0;
    }

    unload_libmediandk(h264);

    free(sys);
    h264->pSystemData = NULL;
}

static BOOL mediacodec_init(H264_CONTEXT* h264)
{
    H264_CONTEXT_MEDIACODEC* sys;

    WINPR_ASSERT(h264);

    // 在 h264_context_init() 中添加（或在你调用 H264_CONTEXT 的地方）
    h264->width = 640;
    h264->height = 480;
    h264->FrameRate = 30;
    h264->BitRate = 5000000;

    if (h264->Compressor)
    {
        WLog_Print(h264->log, WLOG_ERROR, "MediaCodec is not supported as an encoder");
        return FALSE;
    }

    WLog_Print(h264->log, WLOG_DEBUG, "MediaCodec: Initializing context (lazy init)");

    sys = (H264_CONTEXT_MEDIACODEC*)calloc(1, sizeof(H264_CONTEXT_MEDIACODEC));
    if (!sys)
        return FALSE;

    h264->pSystemData = (void*)sys;

    if (load_libmediandk(h264) < 0)
    {
        free(sys);
        h264->pSystemData = NULL;
        return FALSE;
    }

    sys->currentOutputBufferIndex = -1;
    sys->width = sys->height = 0;
    sys->lastWidth = sys->lastHeight = -1;
    sys->formatChanged = FALSE;
    sys->cachedOutputBuffer = NULL;
    sys->cachedOutputBufferSize = 0;

    // ✅ 不创建解码器！延迟到第一次解码时创建
    sys->decoder = NULL;
    sys->inputFormat = NULL;
    sys->outputFormat = NULL;

    WLog_Print(h264->log, WLOG_DEBUG, "MediaCodec context created, waiting for valid width/height");
    return TRUE;
}

H264_CONTEXT_SUBSYSTEM g_Subsystem_mediacodec = { "MediaCodec", mediacodec_init, mediacodec_uninit,
                                                  mediacodec_decompress, mediacodec_compress };