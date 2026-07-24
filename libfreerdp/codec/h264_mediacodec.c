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

/*
 * Android MediaCodec H.264 解码后端(重构版)
 *
 * 设计要点(为什么这么写):
 *
 * 1. 单线程同步 —— FreeRDP 的解码契约是强同步:avc420_decompress() 调本
 *    Decompress 后,立刻在同一线程用 h264->pYUVData 做 YUV→RGB。因此
 *    decode(k) 必须在 Decompress 返回前完成,且 input(k+1) 要等 rgb(k) 跑
 *    完才会到达 —— decode 与 YUV→RGB 在接口层就无法重叠。后台线程在此契约
 *    下换不来重叠,只会徒增线程切换延迟,故不引入。
 *
 * 2. 高效阻塞,不 spin —— 旧实现用 1000ms 超时 + TRY_AGAIN→continue 死等,
 *    任何一次 TRY_AGAIN 就吃掉整整 1 秒,这是"严重卡顿"的直接来源。这里用
 *    100ms 超时(就绪即早返回,不忙等)+ 有界重试(总预算 ~1s 后放弃),
 *    把灾难性秒级卡顿压成良性轮询。
 *
 * 3. RDP AVC = Constrained Baseline(无 B 帧)—— 解码器收到一个 AU 即可吐
 *    一帧,1:1 阻塞不会出现"需要未来输入才出输出"的死锁。
 *
 * 4. 颜色/stride/crop 从实际输出格式读 —— configure 时请求
 *    COLOR_FormatYUV420Planar,但硬件解码器是否采纳由它决定;真实布局从
 *    getOutputFormat() 读 KEY_STRIDE / KEY_SLICE_HEIGHT / KEY_COLOR_FORMAT /
 *    KEY_CROP_*。按 planar(19) / NV12(21) / NV21(22) / Flexible 分别拷贝,
 *    并裁剪到 crop 矩形。旧实现假设 stride==width 且无 crop,换设备/分辨率
 *    就会斜条纹或越界。
 *
 * 5. copy 到自有 planar 缓冲 —— 用 avc420_ensure_buffer() 分配
 *    h264->pYUVData(stride=显示宽),把解码帧按真实布局转换进去。这样
 *    pYUVData 归属清晰(ensure_buffer 可安全 free/重分配),且能处理
 *    semiplanar→planar 的去交错。代价是每帧 ~1.5MB 拷贝(<1ms),远小于
 *    旧实现的 IPC 卡顿税。
 *
 * 6. 删除 cargo-cult 参数 —— 旧实现塞了一堆厂商私 key 和 max-b-frames/
 *    keyint(那是编码器参数,对解码无意义)。只保留 mime/width/height/
 *    framerate/bitrate/color。
 */

#include <winpr/wlog.h>
#include <winpr/assert.h>
#include <winpr/library.h>
#include <winpr/winpr.h>

#include <freerdp/log.h>
#include <freerdp/codec/h264.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <string.h>

#include "h264.h"

/* OMX 颜色格式值(NDK 未导出这些常量,手写) */
enum
{
	MC_COLOR_FormatYUV420Planar = 19,          /* I420: Y/U/V 三平面分离 */
	MC_COLOR_FormatYUV420SemiPlanar = 21,      /* NV12: Y 平面 + UV 交错 */
	MC_COLOR_FormatYUV420SemiPlanarNV21 = 22,  /* NV21: Y 平面 + VU 交错 */
	MC_COLOR_FormatYUV420Flexible = 0x7f420888
};

/* 每次 dequeue 的超时上限(微秒):就绪即早返回,不忙等 */
#define MC_DEQUEUE_TIMEOUT_US 100000
/* 放弃前的重试次数:总预算 ~1s,避免旧实现的无限 spin */
#define MC_DEQUEUE_MAX_RETRIES 10

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
		rc;                                                                      \
	})

/* 必需的 KEY 字符串变量:缺失则加载失败 */
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

/* 可选的 KEY 字符串变量:缺失静默跳过(老设备无 stride/crop key,走默认值) */
#define RESOLVE_MEDIANDK_VARIABLE_OPTIONAL(sys, member, exported)                       \
	do                                                                                  \
	{                                                                                   \
		const char** temp = GetProcAddress(sys->mediandkLibrary, exported);             \
		if (temp != NULL)                                                               \
			sys->member = *temp;                                                        \
	} while (0)

typedef AMediaFormat* (*AMediaFormat_new_t)(void);
typedef media_status_t (*AMediaFormat_delete_t)(AMediaFormat*);
typedef void (*AMediaFormat_setInt32_t)(AMediaFormat*, const char*, int32_t);
typedef BOOL (*AMediaFormat_getInt32_t)(AMediaFormat*, const char*, int32_t*);
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

struct _H264_CONTEXT_MEDIACODEC
{
	AMediaCodec* decoder;
	AMediaFormat* inputFormat;

	/* configure 时记录的显示尺寸,用于检测分辨率变化 */
	int32_t cfgWidth;
	int32_t cfgHeight;

	/* 从输出格式解析出的真实布局(formatReady=TRUE 才有效) */
	BOOL formatReady;
	int32_t codedWidth;   /* KEY_WIDTH/HEIGHT,含对齐 padding */
	int32_t codedHeight;
	int32_t yStride;      /* Y 平面行步长 */
	int32_t sliceHeight;  /* Y 平面高度(含 padding) */
	int32_t colorFormat;  /* 实际输出颜色格式 */
	int32_t cropLeft;
	int32_t cropTop;
	int32_t cropRight;
	int32_t cropBottom;

	/* libmediandk.so imports */
	HMODULE mediandkLibrary;

	AMediaFormat_new_t fnAMediaFormat_new;
	AMediaFormat_delete_t fnAMediaFormat_delete;
	AMediaFormat_setInt32_t fnAMediaFormat_setInt32;
	AMediaFormat_getInt32_t fnAMediaFormat_getInt32;
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

	/* AMEDIAFORMAT_KEY_* 字符串(运行时从 libmediandk.so 解析) */
	const char* gAMediaFormatKeyMime;
	const char* gAMediaFormatKeyWidth;
	const char* gAMediaFormatKeyHeight;
	const char* gAMediaFormatKeyFrameRate;
	const char* gAMediaFormatKeyBitRate;
	const char* gAMediaFormatKeyColorFormat;
	const char* gAMediaFormatKeyStride;
	const char* gAMediaFormatKeySliceHeight;
	const char* gAMediaFormatKeyCropLeft;
	const char* gAMediaFormatKeyCropTop;
	const char* gAMediaFormatKeyCropRight;
	const char* gAMediaFormatKeyCropBottom;
};

typedef struct _H264_CONTEXT_MEDIACODEC H264_CONTEXT_MEDIACODEC;

static int load_libmediandk(H264_CONTEXT* h264)
{
	BOOL rc;
	H264_CONTEXT_MEDIACODEC* sys;

	WINPR_ASSERT(h264);
	sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
	WINPR_ASSERT(sys);

	sys->mediandkLibrary = LoadLibraryA("libmediandk.so");
	if (sys->mediandkLibrary == NULL)
	{
		WLog_Print(h264->log, WLOG_WARN, "Error loading libmediandk.so");
		return -1;
	}

/* 函数符号(全部 API 21+,必需) */
#define R(name)                       \
	do                                \
	{                                 \
		rc = RESOLVE_MEDIANDK_FUNC(sys, name); \
		if (!rc)                      \
			return -1;                \
	} while (0)
	R(AMediaFormat_new);
	R(AMediaFormat_delete);
	R(AMediaFormat_setInt32);
	R(AMediaFormat_getInt32);
	R(AMediaFormat_setString);
	R(AMediaCodec_createDecoderByType);
	R(AMediaCodec_delete);
	R(AMediaCodec_configure);
	R(AMediaCodec_start);
	R(AMediaCodec_stop);
	R(AMediaCodec_getInputBuffer);
	R(AMediaCodec_getOutputBuffer);
	R(AMediaCodec_dequeueInputBuffer);
	R(AMediaCodec_queueInputBuffer);
	R(AMediaCodec_dequeueOutputBuffer);
	R(AMediaCodec_getOutputFormat);
	R(AMediaCodec_releaseOutputBuffer);
#undef R

	/* 必需 KEY:mime/width/height */
	rc = RESOLVE_MEDIANDK_VARIABLE(sys, gAMediaFormatKeyMime, "AMEDIAFORMAT_KEY_MIME");
	if (!rc)
		return -1;
	rc = RESOLVE_MEDIANDK_VARIABLE(sys, gAMediaFormatKeyWidth, "AMEDIAFORMAT_KEY_WIDTH");
	if (!rc)
		return -1;
	rc = RESOLVE_MEDIANDK_VARIABLE(sys, gAMediaFormatKeyHeight, "AMEDIAFORMAT_KEY_HEIGHT");
	if (!rc)
		return -1;

	/* 可选 KEY:缺失走默认值,不致命 */
	RESOLVE_MEDIANDK_VARIABLE_OPTIONAL(sys, gAMediaFormatKeyFrameRate, "AMEDIAFORMAT_KEY_FRAME_RATE");
	RESOLVE_MEDIANDK_VARIABLE_OPTIONAL(sys, gAMediaFormatKeyBitRate, "AMEDIAFORMAT_KEY_BIT_RATE");
	RESOLVE_MEDIANDK_VARIABLE_OPTIONAL(sys, gAMediaFormatKeyColorFormat,
	                                   "AMEDIAFORMAT_KEY_COLOR_FORMAT");
	RESOLVE_MEDIANDK_VARIABLE_OPTIONAL(sys, gAMediaFormatKeyStride, "AMEDIAFORMAT_KEY_STRIDE");
	RESOLVE_MEDIANDK_VARIABLE_OPTIONAL(sys, gAMediaFormatKeySliceHeight,
	                                   "AMEDIAFORMAT_KEY_SLICE_HEIGHT");
	RESOLVE_MEDIANDK_VARIABLE_OPTIONAL(sys, gAMediaFormatKeyCropLeft, "AMEDIAFORMAT_KEY_CROP_LEFT");
	RESOLVE_MEDIANDK_VARIABLE_OPTIONAL(sys, gAMediaFormatKeyCropTop, "AMEDIAFORMAT_KEY_CROP_TOP");
	RESOLVE_MEDIANDK_VARIABLE_OPTIONAL(sys, gAMediaFormatKeyCropRight, "AMEDIAFORMAT_KEY_CROP_RIGHT");
	RESOLVE_MEDIANDK_VARIABLE_OPTIONAL(sys, gAMediaFormatKeyCropBottom,
	                                   "AMEDIAFORMAT_KEY_CROP_BOTTOM");

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
	sys->mediandkLibrary = NULL;
}

/* 解析输出格式:记录 coded 尺寸 / stride / slice-height / 颜色 / crop */
static void parse_output_format(H264_CONTEXT* h264, AMediaFormat* fmt)
{
	H264_CONTEXT_MEDIACODEC* sys;
	int32_t w = 0, h = 0, stride = 0, slice = 0, color = 0;
	int32_t cl = 0, ct = 0, cr = 0, cb = 0;

	WINPR_ASSERT(h264);
	WINPR_ASSERT(fmt);
	sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
	WINPR_ASSERT(sys);

	sys->fnAMediaFormat_getInt32(fmt, sys->gAMediaFormatKeyWidth, &w);
	sys->fnAMediaFormat_getInt32(fmt, sys->gAMediaFormatKeyHeight, &h);
	if (sys->gAMediaFormatKeyStride)
		sys->fnAMediaFormat_getInt32(fmt, sys->gAMediaFormatKeyStride, &stride);
	if (sys->gAMediaFormatKeySliceHeight)
		sys->fnAMediaFormat_getInt32(fmt, sys->gAMediaFormatKeySliceHeight, &slice);
	if (sys->gAMediaFormatKeyColorFormat)
		sys->fnAMediaFormat_getInt32(fmt, sys->gAMediaFormatKeyColorFormat, &color);
	if (sys->gAMediaFormatKeyCropLeft)
		sys->fnAMediaFormat_getInt32(fmt, sys->gAMediaFormatKeyCropLeft, &cl);
	if (sys->gAMediaFormatKeyCropTop)
		sys->fnAMediaFormat_getInt32(fmt, sys->gAMediaFormatKeyCropTop, &ct);
	if (sys->gAMediaFormatKeyCropRight)
		sys->fnAMediaFormat_getInt32(fmt, sys->gAMediaFormatKeyCropRight, &cr);
	if (sys->gAMediaFormatKeyCropBottom)
		sys->fnAMediaFormat_getInt32(fmt, sys->gAMediaFormatKeyCropBottom, &cb);

	if (w <= 0 || h <= 0)
	{
		/* 异常:拿不到尺寸,标记未就绪,等下一帧再试 */
		sys->formatReady = FALSE;
		return;
	}

	sys->codedWidth = w;
	sys->codedHeight = h;
	sys->yStride = (stride > 0) ? stride : w;
	sys->sliceHeight = (slice > 0) ? slice : h;
	sys->colorFormat = color;

	if (cr > cl && cb > ct)
	{
		sys->cropLeft = cl;
		sys->cropTop = ct;
		sys->cropRight = cr;
		sys->cropBottom = cb;
	}
	else
	{
		/* 无 crop key:整个 coded 区域即显示区域 */
		sys->cropLeft = 0;
		sys->cropTop = 0;
		sys->cropRight = w - 1;
		sys->cropBottom = h - 1;
	}

	sys->formatReady = TRUE;

	WLog_Print(h264->log, WLOG_DEBUG,
	           "MediaCodec output fmt: coded=%dx%d stride=%d slice=%d color=0x%x crop=[%d,%d,%d,%d]",
	           w, h, sys->yStride, sys->sliceHeight, (unsigned)color, sys->cropLeft, sys->cropTop,
	           sys->cropRight, sys->cropBottom);
}

/*
 * 把解码帧从 codec 输出缓冲(按真实布局)拷贝到 h264->pYUVData(planar I420,
 * stride=显示宽)。处理 planar / NV12 / NV21 / Flexible,并按 crop 裁剪。
 * 返回 FALSE 表示尺寸异常,调用方应丢弃本帧。
 */
static BOOL copy_yuv420_to_context(H264_CONTEXT* h264, const uint8_t* src, size_t srcSize)
{
	H264_CONTEXT_MEDIACODEC* sys;
	int32_t dstW, dstH, yStride, sliceH, uvStride;
	int32_t cw, ch, cl, ct, cl2, ct2;
	BOOL semiplanar;
	size_t yBytes, uvBytes, need;

	WINPR_ASSERT(h264);
	WINPR_ASSERT(src);
	sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
	WINPR_ASSERT(sys);

	/* 目标用 surface 显示尺寸(由 h264_context_reset 设置,上层 region 矩形按此坐标) */
	dstW = (int32_t)h264->width;
	dstH = (int32_t)h264->height;
	if (dstW <= 0 || dstH <= 0 || !sys->formatReady)
		return FALSE;

	yStride = sys->yStride;
	sliceH = sys->sliceHeight;
	cl = sys->cropLeft;
	ct = sys->cropTop;
	if (yStride <= 0)
		yStride = dstW;
	if (sliceH <= 0)
		sliceH = dstH;

	/* 分配/复用 planar 缓冲(stride = dstW,无 padding,上层好算) */
	if (!avc420_ensure_buffer(h264, (UINT32)dstW, (UINT32)dstW, (UINT32)dstH))
		return FALSE;

	switch (sys->colorFormat)
	{
		case MC_COLOR_FormatYUV420SemiPlanar:      /* NV12 */
		case MC_COLOR_FormatYUV420SemiPlanarNV21:  /* NV21 */
		case MC_COLOR_FormatYUV420Flexible:        /* 灵活格式按 semiplanar 处理 */
			semiplanar = TRUE;
			uvStride = yStride; /* UV 交错,行步长 = Y 步长 */
			break;
		default:
			/* COLOR_FormatYUV420Planar(19)及其它:按 planar 处理 */
			semiplanar = FALSE;
			uvStride = yStride / 2; /* U/V 各占半步长 */
			break;
	}

	/* 源缓冲边界检查,避免越界读 */
	yBytes = (size_t)yStride * (size_t)sliceH;
	uvBytes = (size_t)uvStride * (size_t)(sliceH / 2);
	need = semiplanar ? (yBytes + uvBytes) : (yBytes + 2 * uvBytes);
	if (srcSize < need)
	{
		WLog_Print(h264->log, WLOG_ERROR,
		           "output buffer too small: have %zu, need %zu (stride=%d slice=%d)",
		           srcSize, need, yStride, sliceH);
		return FALSE;
	}

	cw = (dstW + 1) / 2;
	ch = (dstH + 1) / 2;
	cl2 = cl / 2;
	ct2 = ct / 2;

	/* Y 平面 */
	{
		const uint8_t* ySrc = src + (size_t)ct * (size_t)yStride + (size_t)cl;
		uint8_t* yDst = h264->pYUVData[0];
		int32_t r;
		for (r = 0; r < dstH; r++)
			memcpy(yDst + (size_t)r * (size_t)dstW, ySrc + (size_t)r * (size_t)yStride,
			       (size_t)dstW);
	}

	if (!semiplanar)
	{
		/* planar:U/V 各自独立平面 */
		const uint8_t* uSrc = src + yBytes + (size_t)ct2 * (size_t)uvStride + (size_t)cl2;
		const uint8_t* vSrc = uSrc + uvBytes;
		uint8_t* uDst = h264->pYUVData[1];
		uint8_t* vDst = h264->pYUVData[2];
		int32_t r;
		for (r = 0; r < ch; r++)
		{
			memcpy(uDst + (size_t)r * (size_t)cw, uSrc + (size_t)r * (size_t)uvStride, (size_t)cw);
			memcpy(vDst + (size_t)r * (size_t)cw, vSrc + (size_t)r * (size_t)uvStride, (size_t)cw);
		}
	}
	else
	{
		/* semiplanar:UV 交错,去交错到 U/V 两个平面 */
		const uint8_t* uv = src + yBytes + (size_t)ct2 * (size_t)uvStride + (size_t)cl;
		uint8_t* uDst = h264->pYUVData[1];
		uint8_t* vDst = h264->pYUVData[2];
		BOOL nv21 = (sys->colorFormat == MC_COLOR_FormatYUV420SemiPlanarNV21);
		int32_t r, c;
		for (r = 0; r < ch; r++)
		{
			const uint8_t* row = uv + (size_t)r * (size_t)uvStride;
			uint8_t* uRow = uDst + (size_t)r * (size_t)cw;
			uint8_t* vRow = vDst + (size_t)r * (size_t)cw;
			for (c = 0; c < cw; c++)
			{
				uint8_t a = row[(size_t)2 * c];
				uint8_t b = row[(size_t)2 * c + 1];
				if (nv21)
				{
					vRow[c] = a;
					uRow[c] = b;
				}
				else
				{
					uRow[c] = a;
					vRow[c] = b;
				}
			}
		}
	}

	return TRUE;
}

/* 懒创建解码器;分辨率变化时重建。返回 1=就绪,0=尺寸未就绪,-1=失败 */
static int ensure_decoder(H264_CONTEXT* h264)
{
	H264_CONTEXT_MEDIACODEC* sys;
	media_status_t status;
	BOOL needCreate = FALSE;

	WINPR_ASSERT(h264);
	sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
	WINPR_ASSERT(sys);

	if (h264->width == 0 || h264->height == 0)
		return 0;

	if (!sys->decoder)
	{
		needCreate = TRUE;
	}
	else if ((int32_t)h264->width != sys->cfgWidth || (int32_t)h264->height != sys->cfgHeight)
	{
		/* 分辨率变化:销毁旧解码器,稍后重建 */
		sys->fnAMediaCodec_stop(sys->decoder);
		sys->fnAMediaCodec_delete(sys->decoder);
		sys->decoder = NULL;
		if (sys->inputFormat)
		{
			sys->fnAMediaFormat_delete(sys->inputFormat);
			sys->inputFormat = NULL;
		}
		sys->formatReady = FALSE;
		needCreate = TRUE;
	}

	if (!needCreate)
		return 1;

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
	sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyWidth, (int32_t)h264->width);
	sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyHeight,
	                             (int32_t)h264->height);
	if (sys->gAMediaFormatKeyFrameRate && h264->FrameRate > 0)
		sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyFrameRate,
		                             (int32_t)h264->FrameRate);
	if (sys->gAMediaFormatKeyBitRate && h264->BitRate > 0)
		sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyBitRate,
		                             (int32_t)h264->BitRate);
	/* 请求 planar 420(实际格式以输出格式为准) */
	if (sys->gAMediaFormatKeyColorFormat)
		sys->fnAMediaFormat_setInt32(sys->inputFormat, sys->gAMediaFormatKeyColorFormat,
		                             MC_COLOR_FormatYUV420Planar);

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

	sys->cfgWidth = (int32_t)h264->width;
	sys->cfgHeight = (int32_t)h264->height;
	sys->formatReady = FALSE;
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
	H264_CONTEXT_MEDIACODEC* sys;
	int rc;

	WINPR_ASSERT(h264);
	WINPR_ASSERT(pSrcData);

	sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;
	WINPR_ASSERT(sys);

	/* 懒创建/按需重建解码器 */
	rc = ensure_decoder(h264);
	if (rc < 0)
		return -1;
	if (rc == 0)
		return 0; /* 尺寸未就绪:跳过本帧(上层 status==0 → 不做 YUV→RGB) */

	/* ---- 喂入完整帧(必要时跨多个 input buffer) ---- */
	{
		UINT32 offset = 0;
		int retries = 0;

		while (offset < SrcSize)
		{
			ssize_t inputBufferId =
			    sys->fnAMediaCodec_dequeueInputBuffer(sys->decoder, MC_DEQUEUE_TIMEOUT_US);
			if (inputBufferId < 0)
			{
				if (inputBufferId == AMEDIACODEC_INFO_TRY_AGAIN_LATER &&
				    retries < MC_DEQUEUE_MAX_RETRIES)
				{
					retries++;
					continue;
				}
				WLog_Print(h264->log, WLOG_ERROR, "dequeueInputBuffer failed: %zd", inputBufferId);
				return -1;
			}
			retries = 0;

			size_t inputBufferSize = 0;
			uint8_t* inputBuffer =
			    sys->fnAMediaCodec_getInputBuffer(sys->decoder, inputBufferId, &inputBufferSize);
			if (!inputBuffer || inputBufferSize == 0)
			{
				WLog_Print(h264->log, WLOG_ERROR, "getInputBuffer failed");
				return -1;
			}

			UINT32 copySize = ((SrcSize - offset) < inputBufferSize) ? (SrcSize - offset)
			                                                         : (UINT32)inputBufferSize;
			memcpy(inputBuffer, pSrcData + offset, copySize);
			offset += copySize;

			media_status_t status = sys->fnAMediaCodec_queueInputBuffer(sys->decoder, inputBufferId,
			                                                            0, copySize, 0, 0);
			if (status != AMEDIA_OK)
			{
				WLog_Print(h264->log, WLOG_ERROR, "queueInputBuffer failed: %d", status);
				return -1;
			}
		}
	}

	/* ---- 取出一帧输出(处理 format-changed,有界重试,不 spin) ---- */
	{
		int retries = 0;

		for (;;)
		{
			AMediaCodecBufferInfo bufferInfo;
			ssize_t outputBufferId = sys->fnAMediaCodec_dequeueOutputBuffer(
			    sys->decoder, &bufferInfo, MC_DEQUEUE_TIMEOUT_US);

			if (outputBufferId >= 0)
			{
				size_t outputBufferSize = 0;
				uint8_t* outputBuffer = sys->fnAMediaCodec_getOutputBuffer(sys->decoder,
				                                                           outputBufferId,
				                                                           &outputBufferSize);
				if (!outputBuffer)
				{
					WLog_Print(h264->log, WLOG_ERROR, "getOutputBuffer failed");
					sys->fnAMediaCodec_releaseOutputBuffer(sys->decoder, outputBufferId, false);
					return -1;
				}

				/* 首帧若没收到 format-changed,这里补读一次 */
				if (!sys->formatReady)
				{
					AMediaFormat* fmt = sys->fnAMediaCodec_getOutputFormat(sys->decoder);
					if (fmt)
					{
						parse_output_format(h264, fmt);
						sys->fnAMediaFormat_delete(fmt);
					}
				}

				const uint8_t* src = outputBuffer + bufferInfo.offset;
				BOOL ok = copy_yuv420_to_context(h264, src, bufferInfo.size);
				/* 拷贝完立即归还 output buffer(copy 模式下不跨调用持有) */
				sys->fnAMediaCodec_releaseOutputBuffer(sys->decoder, outputBufferId, false);

				if (!ok)
				{
					WLog_Print(h264->log, WLOG_ERROR, "copy_yuv420_to_context failed");
					return -1;
				}
				return 1;
			}
			else if (outputBufferId == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
			{
				AMediaFormat* fmt = sys->fnAMediaCodec_getOutputFormat(sys->decoder);
				if (fmt)
				{
					parse_output_format(h264, fmt);
					sys->fnAMediaFormat_delete(fmt);
				}
				continue;
			}
			else if (outputBufferId == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
			{
				continue;
			}
			else if (outputBufferId == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
			{
				if (retries++ < MC_DEQUEUE_MAX_RETRIES)
					continue;
				WLog_Print(h264->log, WLOG_WARN,
				           "dequeueOutputBuffer timeout after %d retries", MC_DEQUEUE_MAX_RETRIES);
				return -1;
			}
			else
			{
				WLog_Print(h264->log, WLOG_ERROR, "dequeueOutputBuffer failed: %zd", outputBufferId);
				return -1;
			}
		}
	}
}

static void mediacodec_uninit(H264_CONTEXT* h264)
{
	H264_CONTEXT_MEDIACODEC* sys;

	WINPR_ASSERT(h264);
	sys = (H264_CONTEXT_MEDIACODEC*)h264->pSystemData;

	if (!sys)
		return;

	if (sys->decoder)
	{
		media_status_t status = sys->fnAMediaCodec_stop(sys->decoder);
		if (status != AMEDIA_OK)
			WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_stop %d", status);

		status = sys->fnAMediaCodec_delete(sys->decoder);
		if (status != AMEDIA_OK)
			WLog_Print(h264->log, WLOG_ERROR, "Error AMediaCodec_delete %d", status);

		sys->decoder = NULL;
	}

	if (sys->inputFormat)
	{
		sys->fnAMediaFormat_delete(sys->inputFormat);
		sys->inputFormat = NULL;
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

	/* 解码器延迟到 ensure_decoder() 创建(此时还没有宽高) */
	sys->decoder = NULL;
	sys->inputFormat = NULL;
	sys->cfgWidth = 0;
	sys->cfgHeight = 0;
	sys->formatReady = FALSE;

	return TRUE;
}

H264_CONTEXT_SUBSYSTEM g_Subsystem_mediacodec = { "MediaCodec", mediacodec_init, mediacodec_uninit,
                                                  mediacodec_decompress, mediacodec_compress };
