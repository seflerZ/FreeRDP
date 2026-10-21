/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * H.264 Bitmap Compression
 *
 * Copyright 2014 Mike McDonald <Mike.McDonald@software.dell.com>
 * Copyright 2017 David Fort <contact@hardening-consulting.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <winpr/crt.h>
#include <winpr/print.h>
#include <winpr/library.h>
#include <winpr/bitstream.h>
#include <winpr/synch.h>

#include <freerdp/primitives.h>
#include <freerdp/codec/h264.h>
#include <freerdp/log.h>

#include "h264.h"
#include "../mc_probe.h"

#define TAG FREERDP_TAG("codec")

/* 领先一帧:开启时先吐上一帧再喂本帧,把硬解等待藏到渲染后面,代价是固定
   多一帧延迟(60fps 下 ~16.7ms)。0 = 低延迟,每帧多等一次硬解。 */
#ifndef AVC420_LOOKAHEAD
#define AVC420_LOOKAHEAD 1
#endif

static BOOL avc444_ensure_buffer(H264_CONTEXT* h264, DWORD nDstHeight);

BOOL avc420_ensure_buffer(H264_CONTEXT* h264, UINT32 stride, UINT32 width, UINT32 height)
{
	if (!h264)
		return FALSE;

	if (stride == 0)
		stride = width;

	if (stride % 16 != 0)
		stride += 16 - stride % 16;

	if (height % 16 != 0)
		height += 16 - height % 16;

	if (!h264->pYUVData[0] || !h264->pYUVData[1] || !h264->pYUVData[2] || (width != h264->width) ||
	    (height != h264->height) || (stride != h264->iStride[0]))
	{
		h264->iStride[0] = stride;
		h264->iStride[1] = (stride + 1) / 2;
		h264->iStride[2] = (stride + 1) / 2;
		h264->width = width;
		h264->height = height;
		_aligned_free(h264->pYUVData[0]);
		_aligned_free(h264->pYUVData[1]);
		_aligned_free(h264->pYUVData[2]);
		h264->pYUVData[0] = _aligned_malloc(1ull * h264->iStride[0] * height, 16);
		h264->pYUVData[1] = _aligned_malloc(1ull * h264->iStride[1] * height, 16);
		h264->pYUVData[2] = _aligned_malloc(1ull * h264->iStride[2] * height, 16);

		if (!h264->pYUVData[0] || !h264->pYUVData[1] || !h264->pYUVData[2])
			return FALSE;
	}

	return TRUE;
}

static BOOL check_rect(const H264_CONTEXT* h264, const RECTANGLE_16* rect, UINT32 nDstWidth,
                       UINT32 nDstHeight)
{
	/* Check, if the output rectangle is valid in decoded h264 frame. */
	if ((rect->right > h264->width) || (rect->left > h264->width))
		return FALSE;

	if ((rect->top > h264->height) || (rect->bottom > h264->height))
		return FALSE;

	/* Check, if the output rectangle is valid in destination buffer. */
	if ((rect->right > nDstWidth) || (rect->left > nDstWidth))
		return FALSE;

	if ((rect->bottom > nDstHeight) || (rect->top > nDstHeight))
		return FALSE;

	return TRUE;
}

static BOOL avc_yuv_to_rgb(H264_CONTEXT* h264, const RECTANGLE_16* regionRects,
                           UINT32 numRegionRects, UINT32 nDstWidth, UINT32 nDstHeight,
                           UINT32 nDstStep, BYTE* pDstData, DWORD DstFormat, BOOL use444)
{
	UINT32 x;
	BYTE* pDstPoint;
	prim_size_t roi;
	INT32 width, height;
	const BYTE* pYUVPoint[3];
	primitives_t* prims = primitives_get();
#if MC_PROBE
	const double w0 = mc_now_us();
	const double c0 = mc_now_cpu_us();
#endif

	for (x = 0; x < numRegionRects; x++)
	{
		const RECTANGLE_16* rect = &(regionRects[x]);
		const UINT32* iStride;
		BYTE** ppYUVData;

		if (use444)
		{
			iStride = h264->iYUV444Stride;
			ppYUVData = h264->pYUV444Data;
		}
		else
		{
			iStride = h264->iStride;
			ppYUVData = h264->pYUVData;
		}

		if (!check_rect(h264, rect, nDstWidth, nDstHeight))
			return FALSE;

		width = rect->right - rect->left;
		height = rect->bottom - rect->top;
		pDstPoint = pDstData + rect->top * nDstStep + rect->left * 4;
		pYUVPoint[0] = ppYUVData[0] + rect->top * iStride[0] + rect->left;
		pYUVPoint[1] = ppYUVData[1];
		pYUVPoint[2] = ppYUVData[2];

		if (use444)
		{
			pYUVPoint[1] += rect->top * iStride[1] + rect->left;
			pYUVPoint[2] += rect->top * iStride[2] + rect->left;
		}
		else
		{
			pYUVPoint[1] += rect->top / 2 * iStride[1] + rect->left / 2;
			pYUVPoint[2] += rect->top / 2 * iStride[2] + rect->left / 2;
		}

		roi.width = width;
		roi.height = height;

		if (use444)
		{
			if (prims->YUV444ToRGB_8u_P3AC4R(pYUVPoint, iStride, pDstPoint, nDstStep, DstFormat,
			                                 &roi) != PRIMITIVES_SUCCESS)
			{
				return FALSE;
			}
		}
		else
		{
			if (prims->YUV420ToRGB_8u_P3AC4R(pYUVPoint, iStride, pDstPoint, nDstStep, DstFormat,
			                                 &roi) != PRIMITIVES_SUCCESS)
				return FALSE;
		}
	}

	/* wall = YUV→RGB 段的墙钟;cpu = 其中本线程真正吃掉的 CPU。
	   ratio 接近 1 ⇒ 纯算力问题;明显大于 1 ⇒ 相当一部分时间是被调度出去/抢内存带宽
	   (流水线之后硬解在同时干活,这正是新引入的竞争)。
	   generic=1 ⇒ AUTODETECT 选的还是标量实现,NEON 没接上。 */
#if MC_PROBE
	{
		static UINT64 yn = 0;
		static double yWall = 0, yCpu = 0;

		yn++;
		yWall += mc_now_us() - w0;
		yCpu += mc_now_cpu_us() - c0;
		if ((yn % 50) == 0)
			mc_log("YUV n=%llu wall=%.0fus cpu=%.0fus ratio=%.2f generic=%d core=%d",
			       (unsigned long long)yn, yWall / yn, yCpu / yn,
			       (yCpu > 0) ? (yWall / yCpu) : 0.0,
			       (prims->YUV420ToRGB_8u_P3AC4R ==
			        primitives_get_generic()->YUV420ToRGB_8u_P3AC4R) ? 1 : 0,
			       mc_cur_cpu());
	}
#endif

	return TRUE;
}

void h264_lookahead_reset(H264_CONTEXT* h264)
{
	if (!h264)
		return;

	h264->queuedAUs = 0;
	h264->pendingNumRects = 0;
	h264->convertedNumRects = 0;
}

/* 把本帧的 rects 抄进 dst。超过容量就退化成所有 rects 的包围盒 —— 标脏宁可多刷,
   不可漏刷。返回实际存下的 rects 数。 */
static UINT32 h264_store_rects(RECTANGLE_16* dst, const RECTANGLE_16* rects, UINT32 num)
{
	UINT32 i;

	if (!dst || !rects || !num)
		return 0;

	if (num <= H264_MAX_REGION_RECTS)
	{
		memcpy(dst, rects, num * sizeof(RECTANGLE_16));
		return num;
	}

	dst[0] = rects[0];

	for (i = 1; i < num; i++)
	{
		dst[0].left = MIN(dst[0].left, rects[i].left);
		dst[0].top = MIN(dst[0].top, rects[i].top);
		dst[0].right = MAX(dst[0].right, rects[i].right);
		dst[0].bottom = MAX(dst[0].bottom, rects[i].bottom);
	}

	return 1;
}

/* 把在途的那帧吐出来并转换(只取不喂)。返回是否真的转换出了一帧。
   取不到不是错误:保留在途状态,等下一次 tick 再试。 */
BOOL avc420_flush_pending(H264_CONTEXT* h264, BYTE* pDstData, DWORD DstFormat, UINT32 nDstStep,
                          UINT32 nDstWidth, UINT32 nDstHeight)
{
	int status;

	if (!h264)
		return FALSE;

	h264->convertedNumRects = 0;

	if (!h264->lookahead || !h264->pendingNumRects || !h264->queuedAUs)
		return FALSE;

	/* SrcSize==0 = 只取不喂 */
	status = h264->subsystem->Decompress(h264, NULL, 0);

	if (status != 1)
		return FALSE; /* 还没解完 */

	if (!avc_yuv_to_rgb(h264, h264->pendingRects, h264->pendingNumRects, nDstWidth, nDstHeight,
	                    nDstStep, pDstData, DstFormat, FALSE))
	{
		h264_lookahead_reset(h264);
		return FALSE;
	}

	/* 轮换缓冲,保持 convertedRects 与 pendingRects 始终指向不同的缓冲 */
	{
		RECTANGLE_16* const freeBuf = h264->convertedRects;

		h264->convertedRects = h264->pendingRects;
		h264->convertedNumRects = h264->pendingNumRects;
		h264->pendingRects = freeBuf;
		h264->pendingNumRects = 0;
	}

	h264->queuedAUs = 0;
	return TRUE;
}

INT32 avc420_decompress(H264_CONTEXT* h264, const BYTE* pSrcData, UINT32 SrcSize, BYTE* pDstData,
                        DWORD DstFormat, UINT32 nDstStep, UINT32 nDstWidth, UINT32 nDstHeight,
                        RECTANGLE_16* regionRects, UINT32 numRegionRects)
{
	int status;
#if MC_PROBE
	double t0, t1, t2;
#endif

	if (!h264)
		return -1001;

	/* 保险:正常情况下 h264_context_new 已经设好,这里兜住任何别的构造路径 */
	if (!h264->pendingRects || !h264->convertedRects)
	{
		h264->pendingRects = h264->rectBuf[0];
		h264->convertedRects = h264->rectBuf[1];
		h264->pendingNumRects = 0;
		h264->convertedNumRects = 0;
	}

	/* 只有 MediaCodec 后端实现了 dequeue-then-feed 的领先一帧语义 */
	h264->lookahead = AVC420_LOOKAHEAD && (h264->subsystem->name != NULL) &&
	                  (strcmp(h264->subsystem->name, "MediaCodec") == 0);

	MC_MARK(t0);
	status = h264->subsystem->Decompress(h264, pSrcData, SrcSize);
	MC_MARK(t1);

	h264->convertedNumRects = 0;

	if (status < 0)
	{
		/* 出错,像素流断档:丢掉在途状态重新同步 */
		h264_lookahead_reset(h264);
		return status;
	}

	if (status == 0)
	{
		/* 本帧压根没进解码器,同理重新同步 */
		h264_lookahead_reset(h264);
		return 1;
	}

	if (h264->lookahead)
	{
		RECTANGLE_16* const freeBuf = h264->convertedRects; /* 上一轮 gfx.c 已经读走 */
		RECTANGLE_16* const inFlight = h264->pendingRects;  /* 在途 AU 的 rects */
		const UINT32 nInFlight = h264->pendingNumRects;
		BOOL ok = TRUE;

		/* 先把本帧的 rects 抄进闲置缓冲:先存后转,转换失败也不会破坏在途状态 */
		const UINT32 nNew = h264_store_rects(freeBuf, regionRects, numRegionRects);

		/* status==2 表示"喂进去了但这次没吐出来"(首发那帧,或刚被 flush 过),此时
		   没有任何像素被转换。status==1 时吐出来的那帧,就是上一次喂进去的在途帧,
		   pYUVData 里是它的像素 —— 所以要用 inFlight 这组 rects 去转换和标脏。 */
		if ((status == 1) && nInFlight)
		{
			ok = avc_yuv_to_rgb(h264, inFlight, nInFlight, nDstWidth, nDstHeight, nDstStep,
			                    pDstData, DstFormat, FALSE);

			if (ok)
				h264->convertedNumRects = nInFlight;
		}

		/* 缓冲轮换:convertedRects 和 pendingRects 必须永远指向不同的缓冲,否则下次
		   调用写新 rects 时会先把在途帧的 rects 冲掉。转换失败时 convertedNumRects
		   保持 0,gfx.c 不会标脏。 */
		h264->convertedRects = inFlight;
		h264->pendingRects = freeBuf;
		h264->pendingNumRects = nNew;

		if (!ok)
			return -1002;
	}
	else if (!avc_yuv_to_rgb(h264, regionRects, numRegionRects, nDstWidth, nDstHeight, nDstStep,
	                         pDstData, DstFormat, FALSE))
		return -1002;

	MC_MARK(t2);

	/* period = 相邻两次解码入口的间隔,即整帧墙钟(含等网络/解析/前面所有阶段)。
	   判据:period ≈ decomp+yuv+gfx ⇒ 客户端算力受限;period 明显更大 ⇒ 主要卡在等服务端。
	   只在间隔 <200ms 时累加,避免拖一下停一下把空闲时间算进平均。 */
#if MC_PROBE
	{
		static UINT64 n = 0, nPeriod = 0, nJit = 0;
		static double prevT0 = 0, accPeriod = 0, accDec = 0, accYuv = 0, accOwn = 0;
		static double maxPeriod = 0;

		n++;
		accDec += t1 - t0;
		accYuv += t2 - t1;
		accOwn += t2 - t0;
		if (prevT0 > 0)
		{
			const double dt = t0 - prevT0;

			if (dt < 200000.0)
			{
				accPeriod += dt;
				nPeriod++;
				/* 抖动计数:周期超过 25ms 的帧(60fps 该是 16.7ms)。
				   和 vsync 漂移区分 —— 漂移只会让个别帧多等一个 vsync(~25ms),
				   解码器卡顿会出现 40ms+ 的尖峰,所以同时记最大值。 */
				if (dt > 25000.0)
					nJit++;
				if (dt > maxPeriod)
					maxPeriod = dt;
			}
		}
		prevT0 = t0;

		if ((n % 50) == 0)
			mc_log("AVC420 n=%llu period=%.0fus decomp=%.0fus yuv=%.0fus own=%.0fus jit=%llu/%llu max=%.0fus",
			       (unsigned long long)n, nPeriod ? accPeriod / (double)nPeriod : 0.0, accDec / n,
			       accYuv / n, accOwn / n, (unsigned long long)nJit, (unsigned long long)nPeriod,
			       maxPeriod);
	}
#endif

	return 1;
}

INT32 avc420_compress(H264_CONTEXT* h264, const BYTE* pSrcData, DWORD SrcFormat, UINT32 nSrcStep,
                      UINT32 nSrcWidth, UINT32 nSrcHeight, BYTE** ppDstData, UINT32* pDstSize)
{
	prim_size_t roi;
	primitives_t* prims = primitives_get();

	if (!h264)
		return -1;

	if (!h264->subsystem->Compress)
		return -1;

	if (!avc420_ensure_buffer(h264, nSrcStep, nSrcWidth, nSrcHeight))
		return -1;

	roi.width = nSrcWidth;
	roi.height = nSrcHeight;

	if (prims->RGBToYUV420_8u_P3AC4R(pSrcData, SrcFormat, nSrcStep, h264->pYUVData, h264->iStride,
	                                 &roi) != PRIMITIVES_SUCCESS)
		return -1;

	{
		const BYTE* pYUVData[3] = { h264->pYUVData[0], h264->pYUVData[1], h264->pYUVData[2] };
		return h264->subsystem->Compress(h264, pYUVData, h264->iStride, ppDstData, pDstSize);
	}
}

INT32 avc444_compress(H264_CONTEXT* h264, const BYTE* pSrcData, DWORD SrcFormat, UINT32 nSrcStep,
                      UINT32 nSrcWidth, UINT32 nSrcHeight, BYTE version, BYTE* op, BYTE** ppDstData,
                      UINT32* pDstSize, BYTE** ppAuxDstData, UINT32* pAuxDstSize)
{
	prim_size_t roi;
	primitives_t* prims = primitives_get();
	BYTE* coded;
	UINT32 codedSize;

	if (!h264)
		return -1;

	if (!h264->subsystem->Compress)
		return -1;

	if (!avc420_ensure_buffer(h264, nSrcStep, nSrcWidth, nSrcHeight))
		return -1;

	if (!avc444_ensure_buffer(h264, nSrcHeight))
		return -1;

	roi.width = nSrcWidth;
	roi.height = nSrcHeight;

	switch (version)
	{
		case 1:
			if (prims->RGBToAVC444YUV(pSrcData, SrcFormat, nSrcStep, h264->pYUV444Data,
			                          h264->iStride, h264->pYUVData, h264->iStride,
			                          &roi) != PRIMITIVES_SUCCESS)
				return -1;

			break;

		case 2:
			if (prims->RGBToAVC444YUVv2(pSrcData, SrcFormat, nSrcStep, h264->pYUV444Data,
			                            h264->iStride, h264->pYUVData, h264->iStride,
			                            &roi) != PRIMITIVES_SUCCESS)
				return -1;

			break;

		default:
			return -1;
	}

	{
		const BYTE* pYUV444Data[3] = { h264->pYUV444Data[0], h264->pYUV444Data[1],
			                           h264->pYUV444Data[2] };

		if (h264->subsystem->Compress(h264, pYUV444Data, h264->iStride, &coded, &codedSize) < 0)
			return -1;
	}

	memcpy(h264->lumaData, coded, codedSize);
	*ppDstData = h264->lumaData;
	*pDstSize = codedSize;
	{
		const BYTE* pYUVData[3] = { h264->pYUVData[0], h264->pYUVData[1], h264->pYUVData[2] };

		if (h264->subsystem->Compress(h264, pYUVData, h264->iStride, &coded, &codedSize) < 0)
			return -1;
	}
	*ppAuxDstData = coded;
	*pAuxDstSize = codedSize;
	*op = 0;
	return 0;
}

static BOOL avc444_ensure_buffer(H264_CONTEXT* h264, DWORD nDstHeight)
{
	UINT32 x;
	const UINT32* piMainStride = h264->iStride;
	UINT32* piDstSize = h264->iYUV444Size;
	UINT32* piDstStride = h264->iYUV444Stride;
	BYTE** ppYUVDstData = h264->pYUV444Data;
	const UINT32 pad = nDstHeight % 16;
	UINT32 padDstHeight = nDstHeight; /* Need alignment to 16x16 blocks */

	if (pad != 0)
		padDstHeight += 16 - pad;

	if ((piMainStride[0] != piDstStride[0]) || (piDstSize[0] != piMainStride[0] * padDstHeight))
	{
		for (x = 0; x < 3; x++)
		{
			piDstStride[x] = piMainStride[0];
			piDstSize[x] = piDstStride[x] * padDstHeight;
			_aligned_free(ppYUVDstData[x]);
			ppYUVDstData[x] = _aligned_malloc(piDstSize[x], 16);

			if (!ppYUVDstData[x])
				goto fail;

			memset(ppYUVDstData[x], 0, piDstSize[x]);
		}

		_aligned_free(h264->lumaData);
		h264->lumaData = _aligned_malloc(piDstSize[0] * 4, 16);
	}

	for (x = 0; x < 3; x++)
	{
		if (!ppYUVDstData[x] || (piDstSize[x] == 0) || (piDstStride[x] == 0))
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "YUV buffer not initialized! check your decoder settings");
			goto fail;
		}
	}

	if (!h264->lumaData)
		goto fail;

	return TRUE;
fail:
	_aligned_free(ppYUVDstData[0]);
	_aligned_free(ppYUVDstData[1]);
	_aligned_free(ppYUVDstData[2]);
	_aligned_free(h264->lumaData);
	ppYUVDstData[0] = NULL;
	ppYUVDstData[1] = NULL;
	ppYUVDstData[2] = NULL;
	h264->lumaData = NULL;
	return FALSE;
}

static BOOL avc444_process_rects(H264_CONTEXT* h264, const BYTE* pSrcData, UINT32 SrcSize,
                                 BYTE* pDstData, UINT32 DstFormat, UINT32 nDstStep,
                                 UINT32 nDstWidth, UINT32 nDstHeight, const RECTANGLE_16* rects,
                                 UINT32 nrRects, avc444_frame_type type)
{
	const primitives_t* prims = primitives_get();
	UINT32 x;
	UINT32* piDstStride = h264->iYUV444Stride;
	BYTE** ppYUVDstData = h264->pYUV444Data;
	const UINT32* piStride = h264->iStride;
	const BYTE* const* ppYUVData = (const BYTE* const*)h264->pYUVData;

	/* AVC444 的 op==0 每帧调两次 Decompress 且每次都立刻转换,"落后一帧"语义与它
	   根本不兼容。默认 GfxAVC444 是关的,这里只是防服务端中途切档位。 */
	h264->lookahead = FALSE;

	if (h264->subsystem->Decompress(h264, pSrcData, SrcSize) < 0)
		return FALSE;

	if (!avc444_ensure_buffer(h264, nDstHeight))
		return FALSE;

	for (x = 0; x < nrRects; x++)
	{
		const RECTANGLE_16* rect = &rects[x];
		const UINT32 alignedWidth =
		    h264->width + ((h264->width % 16 != 0) ? 16 - h264->width % 16 : 0);
		const UINT32 alignedHeight =
		    h264->height + ((h264->height % 16 != 0) ? 16 - h264->height % 16 : 0);

		if (!check_rect(h264, rect, nDstWidth, nDstHeight))
			continue;

		if (prims->YUV420CombineToYUV444(type, ppYUVData, piStride, alignedWidth, alignedHeight,
		                                 ppYUVDstData, piDstStride, rect) != PRIMITIVES_SUCCESS)
			return FALSE;
	}

	if (!avc_yuv_to_rgb(h264, rects, nrRects, nDstWidth, nDstHeight, nDstStep, pDstData, DstFormat,
	                    TRUE))
		return FALSE;

	return TRUE;
}

#if defined(AVC444_FRAME_STAT)
static UINT64 op1 = 0;
static double op1sum = 0;
static UINT64 op2 = 0;
static double op2sum = 0;
static UINT64 op3 = 0;
static double op3sum = 0;
static double avg(UINT64* count, double old, double size)
{
	double tmp = size + *count * old;
	(*count)++;
	tmp = tmp / *count;
	return tmp;
}
#endif

INT32 avc444_decompress(H264_CONTEXT* h264, BYTE op, RECTANGLE_16* regionRects,
                        UINT32 numRegionRects, const BYTE* pSrcData, UINT32 SrcSize,
                        RECTANGLE_16* auxRegionRects, UINT32 numAuxRegionRect,
                        const BYTE* pAuxSrcData, UINT32 AuxSrcSize, BYTE* pDstData, DWORD DstFormat,
                        UINT32 nDstStep, UINT32 nDstWidth, UINT32 nDstHeight, UINT32 codecId)
{
	INT32 status = -1;
	avc444_frame_type chroma =
	    (codecId == RDPGFX_CODECID_AVC444) ? AVC444_CHROMAv1 : AVC444_CHROMAv2;

	if (!h264 || !regionRects || !pSrcData || !pDstData)
		return -1001;

	switch (op)
	{
		case 0: /* YUV420 in stream 1
		         * Chroma420 in stream 2 */
			if (!avc444_process_rects(h264, pSrcData, SrcSize, pDstData, DstFormat, nDstStep,
			                          nDstWidth, nDstHeight, regionRects, numRegionRects,
			                          AVC444_LUMA))
				status = -1;
			else if (!avc444_process_rects(h264, pAuxSrcData, AuxSrcSize, pDstData, DstFormat,
			                               nDstStep, nDstWidth, nDstHeight, auxRegionRects,
			                               numAuxRegionRect, chroma))
				status = -1;
			else
				status = 0;

			break;

		case 2: /* Chroma420 in stream 1 */
			if (!avc444_process_rects(h264, pSrcData, SrcSize, pDstData, DstFormat, nDstStep,
			                          nDstWidth, nDstHeight, regionRects, numRegionRects, chroma))
				status = -1;
			else
				status = 0;

			break;

		case 1: /* YUV420 in stream 1 */
			if (!avc444_process_rects(h264, pSrcData, SrcSize, pDstData, DstFormat, nDstStep,
			                          nDstWidth, nDstHeight, regionRects, numRegionRects,
			                          AVC444_LUMA))
				status = -1;
			else
				status = 0;

			break;

		default: /* WTF? */
			break;
	}

#if defined(AVC444_FRAME_STAT)

	switch (op)
	{
		case 0:
			op1sum = avg(&op1, op1sum, SrcSize + AuxSrcSize);
			break;

		case 1:
			op2sum = avg(&op2, op2sum, SrcSize);
			break;

		case 2:
			op3sum = avg(&op3, op3sum, SrcSize);
			break;

		default:
			break;
	}

	WLog_Print(h264->log, WLOG_INFO,
	           "luma=%" PRIu64 " [avg=%lf] chroma=%" PRIu64 " [avg=%lf] combined=%" PRIu64
	           " [avg=%lf]",
	           op1, op1sum, op2, op2sum, op3, op3sum);
#endif
	return status;
}

#define MAX_SUBSYSTEMS 10
static INIT_ONCE subsystems_once = INIT_ONCE_STATIC_INIT;
static H264_CONTEXT_SUBSYSTEM* subSystems[MAX_SUBSYSTEMS] = { 0 };

#if defined(_WIN32) && defined(WITH_MEDIA_FOUNDATION)
extern H264_CONTEXT_SUBSYSTEM g_Subsystem_MF;
#endif

static BOOL CALLBACK h264_register_subsystems(PINIT_ONCE once, PVOID param, PVOID* context)
{
	int i = 0;

#ifdef WITH_MEDIACODEC
	{
		extern H264_CONTEXT_SUBSYSTEM g_Subsystem_mediacodec;
		subSystems[i] = &g_Subsystem_mediacodec;
		i++;
	}
#endif
#if defined(_WIN32) && defined(WITH_MEDIA_FOUNDATION)
	{
		subSystems[i] = &g_Subsystem_MF;
		i++;
	}
#endif
#ifdef WITH_OPENH264
	{
		extern H264_CONTEXT_SUBSYSTEM g_Subsystem_OpenH264;
		subSystems[i] = &g_Subsystem_OpenH264;
		i++;
	}
#endif
#ifdef WITH_FFMPEG
	{
		extern H264_CONTEXT_SUBSYSTEM g_Subsystem_libavcodec;
		subSystems[i] = &g_Subsystem_libavcodec;
		i++;
	}
#endif
	return i > 0;
}

static BOOL h264_context_init(H264_CONTEXT* h264)
{
	int i;

	if (!h264)
		return FALSE;

	h264->log = WLog_Get(TAG);

	if (!h264->log)
		return FALSE;

	h264->subsystem = NULL;
	InitOnceExecuteOnce(&subsystems_once, h264_register_subsystems, NULL, NULL);

	for (i = 0; i < MAX_SUBSYSTEMS; i++)
	{
		H264_CONTEXT_SUBSYSTEM* subsystem = subSystems[i];

		if (!subsystem || !subsystem->Init)
			break;

		if (subsystem->Init(h264))
		{
			h264->subsystem = subsystem;
			return TRUE;
		}
	}

	return FALSE;
}

BOOL h264_context_reset(H264_CONTEXT* h264, UINT32 width, UINT32 height)
{
	if (!h264)
		return FALSE;

	h264->width = width;
	h264->height = height;
	/* 尺寸变了,在途帧的 rects 坐标作废 */
	h264_lookahead_reset(h264);
	return TRUE;
}

H264_CONTEXT* h264_context_new(BOOL Compressor)
{
	H264_CONTEXT* h264;
	h264 = (H264_CONTEXT*)calloc(1, sizeof(H264_CONTEXT));

	if (h264)
	{
		h264->Compressor = Compressor;

		/* 领先一帧的两个 rect 缓冲:初始必须指向不同的缓冲,且之后永远成对轮换 */
		h264->pendingRects = h264->rectBuf[0];
		h264->convertedRects = h264->rectBuf[1];

		if (Compressor)
		{
			/* Default compressor settings, may be changed by caller */
			h264->BitRate = 2000000;
			h264->FrameRate = 60;
		}

		if (!h264_context_init(h264))
		{
			free(h264);
			return NULL;
		}
	}

	return h264;
}

void h264_context_free(H264_CONTEXT* h264)
{
	if (h264)
	{
		h264->subsystem->Uninit(h264);
		_aligned_free(h264->pYUV444Data[0]);
		_aligned_free(h264->pYUV444Data[1]);
		_aligned_free(h264->pYUV444Data[2]);
		_aligned_free(h264->lumaData);
		free(h264);
	}
}
