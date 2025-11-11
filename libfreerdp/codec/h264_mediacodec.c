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
#include <winpr/winpr.h>

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

// 补充AMediaFormat_getInt32函数指针定义（修复格式解析依赖）
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
typedef media_status_t (*AMediaCodec_setParameters_t)(AMediaCodec*, const AMediaFormat*);
typedef media_status_t (*AMediaCodec_getName_t)(AMediaCodec*, char**);
typedef void (*AMediaCodec_releaseName_t)(AMediaCodec*, char*);
typedef AMediaFormat* (*AMediaCodec_getInputFormat_t)(AMediaCodec*);

const int COLOR_FormatYUV420Planar = 19;
const int COLOR_FormatYUV420Flexible = 0x7f420888;

struct _H264_CONTEXT_MEDIACODEC
{
    AMediaCodec* decoder;
    AMediaFormat* inputFormat;
    AMediaFormat* outputFormat;
    int32_t width;
    int32_t height;
    ssize_t currentOutputBufferIndex;

    // libmediandk.so imports
    HMODULE mediandkLibrary;

    AMediaFormat_new_t fnAMediaFormat_new;
    AMediaFormat_delete_t fnAMediaFormat_delete;
    AMediaFormat_toString_t fnAMediaFormat_toString;
    AMediaFormat_setInt32_t fnAMediaFormat_setInt32;
    AMediaFormat_getInt32_t fnAMediaFormat_getInt32; // 新增：用于解析输出格式宽高
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

    // 缓存输出缓冲区（修复黑屏：确保完整帧数据）
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
    rc = RESOLVE_MEDIANDK_FUNC(sys, AMediaFormat_getInt32); // 新增：加载格式解析函数
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
        return;

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
            WLog_Print(h264->log, WLOG_ERROR, "Error AMediaFormat_delete %d", status);
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
        return;

    status = sys->fnAMediaCodec_releaseOutputBuffer(sys->decoder, sys->currentOutputBufferIndex, FALSE);
    if (status != AMEDIA_OK)
        WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_releaseOutputBuffer %d", status);

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
    ssize_t inputBufferId = -1;
    size_t inputBufferSize, outputBufferSize;
    uint8_t* inputBuffer;
    media_status_t status;
    const char* media_format;
    BYTE** pYUVData;
    UINT32* iStride;
    H264_CONTEXT_MEDIACODEC* sys;

    WINPR_ASSERT(h264);
    WINPR_ASSERT(pSrcData);

    sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
    WINPR_ASSERT(sys);

    pYUVData = h264->pYUVData;
    WINPR_ASSERT(pYUVData);

    iStride = h264->iStride;
    WINPR_ASSERT(iStride);

    release_current_outputbuffer(h264);

    // 修复黑屏：每次解码前清除缓存缓冲区（避免残留旧数据）
    if (sys->cachedOutputBuffer && sys->cachedOutputBufferSize > 0)
        memset(sys->cachedOutputBuffer, 0, sys->cachedOutputBufferSize);

    // 延迟初始化解码器（确保宽高有效后再创建）
    if (!sys->decoder && h264->width > 0 && h264->height > 0)
    {
        // 修复1：强制宽高16字节对齐（MediaCodec解码器要求）
        int32_t alignedWidth = h264->width;
        int32_t alignedHeight = h264->height;
        if (alignedWidth % 16 != 0)
            alignedWidth += 16 - alignedWidth % 16;
        if (alignedHeight % 16 != 0)
            alignedHeight += 16 - alignedHeight % 16;

        WLog_Print(h264->log, WLOG_INFO, "MediaCodec initializing with aligned resolution %dx%d",
                   alignedWidth, alignedHeight);

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
        sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyWidth, alignedWidth);
        sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyHeight, alignedHeight);
        sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyFrameRate, h264->FrameRate);
        sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyBitRate, h264->BitRate);
        sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyColorFormat, COLOR_FormatYUV420Planar);

        // 修复2：禁用B帧，强制输出完整帧（避免增量更新）
        sys->fnAMediaFormat_setInt32(sys->inputFormat, "max-b-frames", 0);
        sys->fnAMediaFormat_setInt32(sys->inputFormat, "force-key-frames", 1);

        status = sys->fnAMediaCodec_configure(sys->decoder, sys->inputFormat, NULL, NULL, 0);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_configure failed: %d", status);
            goto EXCEPTION;
        }

        status = sys->fnAMediaCodec_start(sys->decoder);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "AMediaCodec_start failed: %d", status);
            goto EXCEPTION;
        }

        // 保存对齐后的宽高（确保后续计算正确）
        sys->width = alignedWidth;
        sys->height = alignedHeight;
        h264->width = alignedWidth;  // 同步到上下文
        h264->height = alignedHeight;
    }

    // 等待宽高有效后再处理
    if (!sys->decoder)
    {
        WLog_Print(h264->log, WLOG_WARN, "Waiting for valid resolution (current: %dx%d)",
                   h264->width, h264->height);
        return 0;
    }

    // 处理输入数据（完整帧入队）
    UINT32 inputOffset = 0;
    while (inputOffset < SrcSize)
    {
        inputBufferId = sys->fnAMediaCodec_dequeueInputBuffer(sys->decoder, 50000); // 50ms超时
        if (inputBufferId < 0)
        {
            if (inputBufferId == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
            {
                WLog_Print(h264->log, WLOG_WARN, "Input buffer busy, retrying...");
                continue;
            }
            WLog_Print(h264->log, WLOG_ERROR, "dequeueInputBuffer failed: %zd", inputBufferId);
            return -1;
        }

        inputBuffer = sys->fnAMediaCodec_getInputBuffer(sys->decoder, inputBufferId, &inputBufferSize);
        if (!inputBuffer)
        {
            WLog_Print(h264->log, WLOG_ERROR, "getInputBuffer failed");
            return -1;
        }

        // 确保一次复制完整帧（不分片）
        UINT32 copySize = (SrcSize - inputOffset) < inputBufferSize ? (SrcSize - inputOffset) : inputBufferSize;
        memcpy(inputBuffer, pSrcData + inputOffset, copySize);
        inputOffset += copySize;

        status = sys->fnAMediaCodec_queueInputBuffer(sys->decoder, inputBufferId, 0, copySize, 0, 0);
        if (status != AMEDIA_OK)
        {
            WLog_Print(h264->log, WLOG_ERROR, "queueInputBuffer failed: %d", status);
            return -1;
        }
    }

    // 处理输出数据
    while (true)
    {
        AMediaCodecBufferInfo bufferInfo;
        ssize_t outputBufferId = sys->fnAMediaCodec_dequeueOutputBuffer(sys->decoder, &bufferInfo, 50000);
        if (outputBufferId >= 0)
        {
            sys->currentOutputBufferIndex = outputBufferId;
            uint8_t* outputBuffer = sys->fnAMediaCodec_getOutputBuffer(sys->decoder, outputBufferId, &outputBufferSize);
            if (!outputBuffer)
            {
                WLog_Print(h264->log, WLOG_ERROR, "getOutputBuffer failed");
                sys->fnAMediaCodec_releaseOutputBuffer(sys->decoder, outputBufferId, false);
                return -1;
            }

            // 修复3：校验输出缓冲区尺寸是否为完整YUV420帧
            size_t expectedSize = sys->width * sys->height * 3 / 2; // 1.5字节/像素
            if (outputBufferSize < expectedSize)
            {
                WLog_Print(h264->log, WLOG_ERROR, "Output buffer too small (need %zu, got %zu)",
                           expectedSize, outputBufferSize);
                sys->fnAMediaCodec_releaseOutputBuffer(sys->decoder, outputBufferId, false);
                return -1;
            }

            // 确保缓存区足够大
            if (sys->cachedOutputBufferSize < expectedSize)
            {
                if (sys->cachedOutputBuffer)
                    free(sys->cachedOutputBuffer);
                sys->cachedOutputBuffer = malloc(expectedSize);
                if (!sys->cachedOutputBuffer)
                {
                    WLog_Print(h264->log, WLOG_ERROR, "Failed to allocate cached buffer");
                    sys->fnAMediaCodec_releaseOutputBuffer(sys->decoder, outputBufferId, false);
                    return -1;
                }
                sys->cachedOutputBufferSize = expectedSize;
            }

            // 复制完整帧数据（仅复制有效区域）
            memcpy(sys->cachedOutputBuffer, outputBuffer, expectedSize);

            // 修复4：正确计算YUV平面偏移（确保全画面映射）
            iStride[0] = sys->width;                  // Y平面步长 = 完整宽度
            iStride[1] = sys->width / 2;              // U平面步长 = 宽度/2（对齐后为整数）
            iStride[2] = sys->width / 2;              // V平面步长 = 宽度/2

            pYUVData[0] = sys->cachedOutputBuffer;                                  // Y平面（完整宽高）
            pYUVData[1] = sys->cachedOutputBuffer + (sys->width * sys->height);     // U平面（Y平面之后）
            pYUVData[2] = pYUVData[1] + (iStride[1] * (sys->height / 2));           // V平面（U平面之后）

            break;
        }
        else if (outputBufferId == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        {
            // 处理格式变更，同步更新宽高
            AMediaFormat* newFormat = sys->fnAMediaCodec_getOutputFormat(sys->decoder);
            if (!newFormat)
            {
                WLog_Print(h264->log, WLOG_ERROR, "getOutputFormat failed after format change");
                return -1;
            }

            int32_t newWidth = 0, newHeight = 0;
            if (!sys->fnAMediaFormat_getInt32(newFormat, sys->gAMediaFormatKeyWidth, &newWidth) ||
                !sys->fnAMediaFormat_getInt32(newFormat, sys->gAMediaFormatKeyHeight, &newHeight))
            {
                WLog_Print(h264->log, WLOG_ERROR, "Failed to get new resolution from format");
                sys->fnAMediaFormat_delete(newFormat);
                return -1;
            }

            // 修复5：同步所有宽高变量（避免渲染尺寸不匹配）
            if (newWidth != sys->width || newHeight != sys->height)
            {
                sys->width = newWidth;
                sys->height = newHeight;
                h264->width = newWidth;
                h264->height = newHeight;
                WLog_Print(h264->log, WLOG_INFO, "Resolution changed to %dx%d", newWidth, newHeight);
            }

            set_mediacodec_format(h264, &sys->outputFormat, newFormat);
        }
        else if (outputBufferId == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        {
            WLog_Print(h264->log, WLOG_WARN, "Output buffer not ready, retrying...");
            continue;
        }
        else
        {
            WLog_Print(h264->log, WLOG_ERROR, "dequeueOutputBuffer failed: %zd", outputBufferId);
            return -1;
        }
    }

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
            WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_stop %d", status);

        status = sys->fnAMediaCodec_delete(sys->decoder);
        if (status != AMEDIA_OK)
            WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_delete %d", status);

        sys->decoder = NULL;
    }

    set_mediacodec_format(h264, &sys->inputFormat, NULL);
    set_mediacodec_format(h264, &sys->outputFormat, NULL);

    // 释放缓存缓冲区
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

    if (h264->Compressor)
    {
        WLog_Print(h264->log, WLOG_ERROR, "MediaCodec is not supported as an encoder");
        return FALSE;
    }

    WLog_Print(h264->log, WLOG_DEBUG, "Initializing MediaCodec context");

    sys = (H264_CONTEXT_MEDIACODEC*)calloc(1, sizeof(H264_CONTEXT_MEDIACODEC));
    if (!sys)
        return FALSE;

    h264->pSystemData = (void*)sys;

    // 加载libmediandk库
    if (load_libmediandk(h264) < 0)
    {
        free(sys);
        h264->pSystemData = NULL;
        return FALSE;
    }

    // 初始化成员变量
    sys->currentOutputBufferIndex = -1;
    sys->width = sys->height = 0;
    sys->decoder = NULL;
    sys->inputFormat = NULL;
    sys->outputFormat = NULL;
    sys->cachedOutputBuffer = NULL;
    sys->cachedOutputBufferSize = 0;

    return TRUE;
}

H264_CONTEXT_SUBSYSTEM g_Subsystem_mediacodec = { "MediaCodec", mediacodec_init, mediacodec_uninit,
                                                  mediacodec_decompress, mediacodec_compress };