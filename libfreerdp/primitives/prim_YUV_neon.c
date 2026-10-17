/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Optimized YUV/RGB conversion operations
 *
 * Copyright 2014 Thomas Erbesdobler
 * Copyright 2016-2017 Armin Novak <armin.novak@thincast.com>
 * Copyright 2016-2017 Norbert Federa <norbert.federa@thincast.com>
 * Copyright 2016-2017 Thincast Technologies GmbH
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* cpu_set_t / pthread_setaffinity_np */
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <winpr/sysinfo.h>
#include <winpr/crt.h>
#include <freerdp/types.h>
#include <freerdp/primitives.h>

#include "prim_internal.h"

#if !defined(WITH_NEON)
#error "This file must only be included if WITH_NEON is active!"
#endif

#include <arm_neon.h>

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

static primitives_t* generic = NULL;

static INLINE uint8x8_t neon_YUV2R(int32x4_t Ch, int32x4_t Cl, int16x4_t Dh, int16x4_t Dl,
                                   int16x4_t Eh, int16x4_t El)
{
	/* R = (256 * Y + 403 * (V - 128)) >> 8 */
	const int16x4_t c403 = vdup_n_s16(403);
	const int32x4_t CEh = vmlal_s16(Ch, Eh, c403);
	const int32x4_t CEl = vmlal_s16(Cl, El, c403);
	const int32x4_t Rh = vrshrq_n_s32(CEh, 8);
	const int32x4_t Rl = vrshrq_n_s32(CEl, 8);
	const int16x8_t R = vcombine_s16(vqmovn_s32(Rl), vqmovn_s32(Rh));
	return vqmovun_s16(R);
}

static INLINE uint8x8_t neon_YUV2G(int32x4_t Ch, int32x4_t Cl, int16x4_t Dh, int16x4_t Dl,
                                   int16x4_t Eh, int16x4_t El)
{
	/* G = (256L * Y -  48 * (U - 128) - 120 * (V - 128)) >> 8 */
	const int16x4_t c48 = vdup_n_s16(48);
	const int16x4_t c120 = vdup_n_s16(120);
	const int32x4_t CDh = vmlsl_s16(Ch, Dh, c48);
	const int32x4_t CDl = vmlsl_s16(Cl, Dl, c48);
	const int32x4_t CDEh = vmlsl_s16(CDh, Eh, c120);
	const int32x4_t CDEl = vmlsl_s16(CDl, El, c120);
	const int32x4_t Gh = vrshrq_n_s32(CDEh, 8);
	const int32x4_t Gl = vrshrq_n_s32(CDEl, 8);
	const int16x8_t G = vcombine_s16(vqmovn_s32(Gl), vqmovn_s32(Gh));
	return vqmovun_s16(G);
}

static INLINE uint8x8_t neon_YUV2B(int32x4_t Ch, int32x4_t Cl, int16x4_t Dh, int16x4_t Dl,
                                   int16x4_t Eh, int16x4_t El)
{
	/* B = (256L * Y + 475 * (U - 128)) >> 8*/
	const int16x4_t c475 = vdup_n_s16(475);
	const int32x4_t CDh = vmlal_s16(Ch, Dh, c475);
	const int32x4_t CDl = vmlal_s16(Ch, Dl, c475);
	const int32x4_t Bh = vrshrq_n_s32(CDh, 8);
	const int32x4_t Bl = vrshrq_n_s32(CDl, 8);
	const int16x8_t B = vcombine_s16(vqmovn_s32(Bl), vqmovn_s32(Bh));
	return vqmovun_s16(B);
}

static INLINE BYTE* neon_YuvToRgbPixel(BYTE* pRGB, int16x8_t Y, int16x8_t D, int16x8_t E,
                                       const uint8_t rPos, const uint8_t gPos, const uint8_t bPos,
                                       const uint8_t aPos)
{
	uint8x8x4_t bgrx;
	const int16x4_t Dl = vget_low_s16(D);
	const int16x4_t Dh = vget_high_s16(D);
	const int16x4_t El = vget_low_s16(E);
	const int16x4_t Eh = vget_high_s16(E);
	const int16x4_t c475 = vdup_n_s16(475);
	const int16x4_t c48n = vdup_n_s16(-48);
	const int16x4_t c120n = vdup_n_s16(-120);
	const int16x4_t c403 = vdup_n_s16(403);
	/* Y*256:vshll 一条指令同时完成「16→32 位加宽」和「左移 8」,
	   替掉原来的 vmovl_s16 + vmulq_n_s32 两条 */
	const int32x4_t Yl8 = vshll_n_s16(vget_low_s16(Y), 8);
	const int32x4_t Yh8 = vshll_n_s16(vget_high_s16(Y), 8);

/* 收尾:vrshrn_n_s32 一条指令完成「四舍五入 >>8 + 收窄到 16 位」,
   替掉原来的 vrshrq_n_s32 + vqmovn_s32(×2) + vcombine_s16 三步。

   这里的值域有界,不需要饱和:三个通道在 `>>8` 之前的范围分别是
     B: [475*(-128), 255*256 + 475*127] = [-60800, 125605]  ⇒ >>8 得 [-238, 490]
     G: [-(48+120)*127, 255*256 + (48+120)*128]           ⇒ >>8 得 [-84, 339]
     R: [403*(-128), 255*256 + 403*127]                   ⇒ >>8 得 [-202, 454]
   全都远在 int16 范围内,所以旧的 vqmovn_s32 那一步从来没有真正夹过值,
   用不饱和的 vrshrn 结果逐位相同(而且是 AArch32/AArch64 都有的指令)。 */
#define NEON_NARROW(lo32, hi32) \
	vqmovun_s16(vcombine_s16(vrshrn_n_s32((lo32), 8), vrshrn_n_s32((hi32), 8)))

	/* B = (256*Y + 475*(U-128)) >> 8 */
	bgrx.val[bPos] = NEON_NARROW(vmlal_s16(Yl8, Dl, c475), vmlal_s16(Yh8, Dh, c475));
	/* G = (256*Y - 48*(U-128) - 120*(V-128)) >> 8 */
	bgrx.val[gPos] = NEON_NARROW(vmlal_s16(vmlal_s16(Yl8, Dl, c48n), El, c120n),
	                             vmlal_s16(vmlal_s16(Yh8, Dh, c48n), Eh, c120n));
	/* R = (256*Y + 403*(V-128)) >> 8 */
	bgrx.val[rPos] = NEON_NARROW(vmlal_s16(Yl8, El, c403), vmlal_s16(Yh8, Eh, c403));
	/* A */
	bgrx.val[aPos] = vdup_n_u8(0xFF);

#undef NEON_NARROW

	vst4_u8(pRGB, bgrx);
	pRGB += 32;
	return pRGB;
}

static INLINE pstatus_t neon_YUV420ToX(const BYTE* const pSrc[3], const UINT32 srcStep[3],
                                       BYTE* pDst, UINT32 dstStep, const prim_size_t* roi,
                                       const uint8_t rPos, const uint8_t gPos, const uint8_t bPos,
                                       const uint8_t aPos)
{
	UINT32 y;
	const UINT32 nWidth = roi->width;
	const UINT32 nHeight = roi->height;
	const DWORD pad = nWidth % 16;
	const UINT32 yPad = srcStep[0] - roi->width;
	const UINT32 uPad = srcStep[1] - roi->width / 2;
	const UINT32 vPad = srcStep[2] - roi->width / 2;
	const UINT32 dPad = dstStep - roi->width * 4;
	const int16x8_t c128 = vdupq_n_s16(128);

	for (y = 0; y < nHeight; y += 2)
	{
		const uint8_t* pY1 = pSrc[0] + y * srcStep[0];
		const uint8_t* pY2 = pY1 + srcStep[0];
		const uint8_t* pU = pSrc[1] + (y / 2) * srcStep[1];
		const uint8_t* pV = pSrc[2] + (y / 2) * srcStep[2];
		uint8_t* pRGB1 = pDst + y * dstStep;
		uint8_t* pRGB2 = pRGB1 + dstStep;
		UINT32 x;
		const BOOL lastY = y >= nHeight - 1;

		for (x = 0; x < nWidth - pad;)
		{
			/* 先把 8 个 U 字节整体加宽到 16 位,再在 16 位上把每个值复制成相邻
			   两份 —— 比原来的 vzip_u8 + vmovl_u8×2 少一条指令,数据布局相同。 */
			const uint16x8_t Uraw = vmovl_u8(vld1_u8(pU));
			const uint16x8x2_t Uu = vzipq_u16(Uraw, Uraw);
			const uint16x8_t Vraw = vmovl_u8(vld1_u8(pV));
			const uint16x8x2_t Vu = vzipq_u16(Vraw, Vraw);
			const int16x8_t D1 = vsubq_s16(vreinterpretq_s16_u16(Uu.val[0]), c128);
			const int16x8_t D2 = vsubq_s16(vreinterpretq_s16_u16(Uu.val[1]), c128);
			const int16x8_t E1 = vsubq_s16(vreinterpretq_s16_u16(Vu.val[0]), c128);
			const int16x8_t E2 = vsubq_s16(vreinterpretq_s16_u16(Vu.val[1]), c128);
			{
				const uint8x8_t Y1u = vld1_u8(pY1);
				const int16x8_t Y1 = vreinterpretq_s16_u16(vmovl_u8(Y1u));
				pRGB1 = neon_YuvToRgbPixel(pRGB1, Y1, D1, E1, rPos, gPos, bPos, aPos);
				pY1 += 8;
				x += 8;
			}
			{
				const uint8x8_t Y1u = vld1_u8(pY1);
				const int16x8_t Y1 = vreinterpretq_s16_u16(vmovl_u8(Y1u));
				pRGB1 = neon_YuvToRgbPixel(pRGB1, Y1, D2, E2, rPos, gPos, bPos, aPos);
				pY1 += 8;
				x += 8;
			}

			if (!lastY)
			{
				{
					const uint8x8_t Y2u = vld1_u8(pY2);
					const int16x8_t Y2 = vreinterpretq_s16_u16(vmovl_u8(Y2u));
					pRGB2 = neon_YuvToRgbPixel(pRGB2, Y2, D1, E1, rPos, gPos, bPos, aPos);
					pY2 += 8;
				}
				{
					const uint8x8_t Y2u = vld1_u8(pY2);
					const int16x8_t Y2 = vreinterpretq_s16_u16(vmovl_u8(Y2u));
					pRGB2 = neon_YuvToRgbPixel(pRGB2, Y2, D2, E2, rPos, gPos, bPos, aPos);
					pY2 += 8;
				}
			}

			pU += 8;
			pV += 8;
		}

		for (; x < nWidth; x++)
		{
			const BYTE U = *pU;
			const BYTE V = *pV;
			{
				const BYTE Y = *pY1++;
				const BYTE r = YUV2R(Y, U, V);
				const BYTE g = YUV2G(Y, U, V);
				const BYTE b = YUV2B(Y, U, V);
				pRGB1[aPos] = 0xFF;
				pRGB1[rPos] = r;
				pRGB1[gPos] = g;
				pRGB1[bPos] = b;
				pRGB1 += 4;
			}

			if (!lastY)
			{
				const BYTE Y = *pY2++;
				const BYTE r = YUV2R(Y, U, V);
				const BYTE g = YUV2G(Y, U, V);
				const BYTE b = YUV2B(Y, U, V);
				pRGB2[aPos] = 0xFF;
				pRGB2[rPos] = r;
				pRGB2[gPos] = g;
				pRGB2[bPos] = b;
				pRGB2 += 4;
			}

			if (x % 2)
			{
				pU++;
				pV++;
			}
		}

		pRGB1 += dPad;
		pRGB2 += dPad;
		pY1 += yPad;
		pY2 += yPad;
		pU += uPad;
		pV += vPad;
	}

	return PRIMITIVES_SUCCESS;
}

/* ---- 分块并行 ----
   YUV→RGB 是单帧最重的纯 CPU 段(1080p 实测 ~6.4ms,吃满一核)。这里用常驻
   worker + 原子取块:每 32 行的块谁空谁领,主线程也算一个 worker。异构核
   (A5xx/A7xx/X3) 上按固定行带切会被最慢的带拖死,取块式天然自平衡。块起点
   都是 32 的倍数 ⇒ 必为偶数行,满足 YUV420 的 U/V 每两行共享。参数只在一帧
   内有效:主线程等 pending 归零才返回,worker 读参不需要额外加锁。 */
#define YUV_PAR_WORKERS 3
#define YUV_PAR_MIN_HEIGHT 192 /* 低于此高度并行不划算 */
#define YUV_PAR_CHUNK_ROWS 32

typedef struct
{
	pthread_mutex_t mu;
	pthread_cond_t work_cv;
	pthread_cond_t done_cv;
	pthread_t workers[YUV_PAR_WORKERS];
	int started;
	unsigned seq;
	int pending;
	int busy;

	const BYTE* src[3];
	UINT32 srcStep[3];
	BYTE* dst;
	UINT32 dstStep;
	UINT32 width;
	UINT32 rows;
	UINT32 nextRow;
	uint8_t rPos, gPos, bPos, aPos;
	int pinCpu[YUV_PAR_WORKERS];
} yuv_par_t;

static yuv_par_t g_yuv_par = {
	PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER,
	{ 0 }, 0, 0, 0, 0, { 0 }, { 0 }, NULL, 0, 0, 0, 0, 0, 0, 0, 0, { -1, -1, -1 }
};

static void yuv_par_chunk(yuv_par_t* p, UINT32 top, UINT32 height)
{
	const BYTE* src[3];
	UINT32 step[3];
	prim_size_t roi;

	src[0] = p->src[0] + (size_t)top * p->srcStep[0];
	src[1] = p->src[1] + (size_t)(top / 2) * p->srcStep[1];
	src[2] = p->src[2] + (size_t)(top / 2) * p->srcStep[2];
	step[0] = p->srcStep[0];
	step[1] = p->srcStep[1];
	step[2] = p->srcStep[2];
	roi.width = p->width;
	roi.height = height;

	neon_YUV420ToX(src, step, p->dst + (size_t)top * p->dstStep, p->dstStep, &roi, p->rPos,
	               p->gPos, p->bPos, p->aPos);
}

static void yuv_par_run(yuv_par_t* p)
{
	for (;;)
	{
		const UINT32 top = __atomic_fetch_add(&p->nextRow, YUV_PAR_CHUNK_ROWS, __ATOMIC_RELAXED);
		UINT32 height;

		if (top >= p->rows)
			break;
		height = p->rows - top;
		if (height > YUV_PAR_CHUNK_ROWS)
			height = YUV_PAR_CHUNK_ROWS;
		yuv_par_chunk(p, top, height);
	}
}

static void* yuv_par_worker(void* arg)
{
	const int idx = (int)(intptr_t)arg;
	unsigned seen = 0;
	char name[16];

	snprintf(name, sizeof(name), "yuv-par%d", idx);
	pthread_setname_np(pthread_self(), name);

	/* bionic 没有 pthread_setaffinity_np,在自己的线程里用 sched_setaffinity */
	if (g_yuv_par.pinCpu[idx] >= 0)
	{
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(g_yuv_par.pinCpu[idx], &set);
		sched_setaffinity(0, sizeof(set), &set);
	}

	pthread_mutex_lock(&g_yuv_par.mu);
	for (;;)
	{
		while (g_yuv_par.seq == seen)
			pthread_cond_wait(&g_yuv_par.work_cv, &g_yuv_par.mu);

		seen = g_yuv_par.seq;
		pthread_mutex_unlock(&g_yuv_par.mu);
		yuv_par_run(&g_yuv_par);
		pthread_mutex_lock(&g_yuv_par.mu);
		if (--g_yuv_par.pending == 0)
			pthread_cond_signal(&g_yuv_par.done_cv);
	}
	pthread_mutex_unlock(&g_yuv_par.mu);
	return NULL;
}

/* 按 cpuinfo_max_freq 找出最快的几颗核。Android 上省电调度很容易把新线程
   丢到 A5xx 小核,一条带就能拖慢整帧;把 worker 绑到大核把方差压下去。
   读不到频点(部分内核/容器)就返回 0,退回不绑核。 */
static int yuv_par_big_cpus(int* cpus, int max)
{
	long freq[32];
	int n = 0;
	int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
	int i, j;

	if (ncpu <= 0)
		return 0;
	if (ncpu > (int)(sizeof(freq) / sizeof(freq[0])))
		ncpu = (int)(sizeof(freq) / sizeof(freq[0]));

	for (i = 0; i < ncpu; i++)
	{
		char path[96];
		char buf[32];
		FILE* f;

		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (!fgets(buf, sizeof(buf), f))
		{
			fclose(f);
			continue;
		}
		fclose(f);
		freq[n] = strtol(buf, NULL, 10);
		cpus[n] = i;
		n++;
	}

	if (n < 2)
		return 0;

	/* 按频点降序排(核数很少,插入排序足够) */
	for (i = 1; i < n; i++)
	{
		const long f = freq[i];
		const int c = cpus[i];

		for (j = i; (j > 0) && (freq[j - 1] < f); j--)
		{
			freq[j] = freq[j - 1];
			cpus[j] = cpus[j - 1];
		}
		freq[j] = f;
		cpus[j] = c;
	}

	/* 同频核都算"大核",只取最快的 max 颗 */
	return (n < max) ? n : max;
}

static void yuv_par_start(void)
{
	int i;
	int cpus[YUV_PAR_WORKERS + 1];
	const int n = yuv_par_big_cpus(cpus, YUV_PAR_WORKERS + 1);

	pthread_mutex_lock(&g_yuv_par.mu);
	for (i = 0; i < YUV_PAR_WORKERS; i++)
	{
		/* cpus[0] 留给主线程(它也领一条带),worker 从第二快的核开始绑 */
		g_yuv_par.pinCpu[i] = (i + 1 < n) ? cpus[i + 1] : -1;
		if (pthread_create(&g_yuv_par.workers[i], NULL, yuv_par_worker, (void*)(intptr_t)i) == 0)
			g_yuv_par.started++;
	}
	pthread_mutex_unlock(&g_yuv_par.mu);
}

static pstatus_t neon_YUV420ToX_par(const BYTE* const pSrc[3], const UINT32 srcStep[3],
                                    BYTE* pDst, UINT32 dstStep, const prim_size_t* roi,
                                    const uint8_t rPos, const uint8_t gPos, const uint8_t bPos,
                                    const uint8_t aPos)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	int workers;

	if (roi->height < YUV_PAR_MIN_HEIGHT)
		return neon_YUV420ToX(pSrc, srcStep, pDst, dstStep, roi, rPos, gPos, bPos, aPos);

	pthread_once(&once, yuv_par_start);

	pthread_mutex_lock(&g_yuv_par.mu);
	workers = g_yuv_par.started;
	/* video 通道等其它调用者可能在另一线程同时进来;池是单帧独占的,
	   这种罕见并发直接退回单线程,别把参数搅在一起。 */
	if ((workers <= 0) || g_yuv_par.busy)
	{
		pthread_mutex_unlock(&g_yuv_par.mu);
		return neon_YUV420ToX(pSrc, srcStep, pDst, dstStep, roi, rPos, gPos, bPos, aPos);
	}
	g_yuv_par.busy = 1;

	g_yuv_par.src[0] = pSrc[0];
	g_yuv_par.src[1] = pSrc[1];
	g_yuv_par.src[2] = pSrc[2];
	g_yuv_par.srcStep[0] = srcStep[0];
	g_yuv_par.srcStep[1] = srcStep[1];
	g_yuv_par.srcStep[2] = srcStep[2];
	g_yuv_par.dst = pDst;
	g_yuv_par.dstStep = dstStep;
	g_yuv_par.width = roi->width;
	g_yuv_par.rows = roi->height;
	g_yuv_par.nextRow = 0;
	g_yuv_par.rPos = rPos;
	g_yuv_par.gPos = gPos;
	g_yuv_par.bPos = bPos;
	g_yuv_par.aPos = aPos;
	g_yuv_par.pending = workers;
	g_yuv_par.seq++;
	pthread_cond_broadcast(&g_yuv_par.work_cv);
	pthread_mutex_unlock(&g_yuv_par.mu);

	yuv_par_run(&g_yuv_par);

	pthread_mutex_lock(&g_yuv_par.mu);
	while (g_yuv_par.pending > 0)
		pthread_cond_wait(&g_yuv_par.done_cv, &g_yuv_par.mu);
	g_yuv_par.busy = 0;
	pthread_mutex_unlock(&g_yuv_par.mu);

	return PRIMITIVES_SUCCESS;
}

static pstatus_t neon_YUV420ToRGB_8u_P3AC4R(const BYTE* const pSrc[3], const UINT32 srcStep[3],
                                            BYTE* pDst, UINT32 dstStep, UINT32 DstFormat,
                                            const prim_size_t* roi)
{
	switch (DstFormat)
	{
		case PIXEL_FORMAT_BGRA32:
		case PIXEL_FORMAT_BGRX32:
			return neon_YUV420ToX_par(pSrc, srcStep, pDst, dstStep, roi, 2, 1, 0, 3);

		case PIXEL_FORMAT_RGBA32:
		case PIXEL_FORMAT_RGBX32:
			return neon_YUV420ToX_par(pSrc, srcStep, pDst, dstStep, roi, 0, 1, 2, 3);

		case PIXEL_FORMAT_ARGB32:
		case PIXEL_FORMAT_XRGB32:
			return neon_YUV420ToX_par(pSrc, srcStep, pDst, dstStep, roi, 1, 2, 3, 0);

		case PIXEL_FORMAT_ABGR32:
		case PIXEL_FORMAT_XBGR32:
			return neon_YUV420ToX_par(pSrc, srcStep, pDst, dstStep, roi, 3, 2, 1, 0);

		default:
			return generic->YUV420ToRGB_8u_P3AC4R(pSrc, srcStep, pDst, dstStep, DstFormat, roi);
	}
}

static INLINE pstatus_t neon_YUV444ToX(const BYTE* const pSrc[3], const UINT32 srcStep[3],
                                       BYTE* pDst, UINT32 dstStep, const prim_size_t* roi,
                                       const uint8_t rPos, const uint8_t gPos, const uint8_t bPos,
                                       const uint8_t aPos)
{
	UINT32 y;
	const UINT32 nWidth = roi->width;
	const UINT32 nHeight = roi->height;
	const UINT32 yPad = srcStep[0] - roi->width;
	const UINT32 uPad = srcStep[1] - roi->width;
	const UINT32 vPad = srcStep[2] - roi->width;
	const UINT32 dPad = dstStep - roi->width * 4;
	const uint8_t* pY = pSrc[0];
	const uint8_t* pU = pSrc[1];
	const uint8_t* pV = pSrc[2];
	uint8_t* pRGB = pDst;
	const int16x8_t c128 = vdupq_n_s16(128);
	const DWORD pad = nWidth % 8;

	for (y = 0; y < nHeight; y++)
	{
		UINT32 x;

		for (x = 0; x < nWidth - pad; x += 8)
		{
			const uint8x8_t Yu = vld1_u8(pY);
			const int16x8_t Y = vreinterpretq_s16_u16(vmovl_u8(Yu));
			const uint8x8_t Uu = vld1_u8(pU);
			const int16x8_t U = vreinterpretq_s16_u16(vmovl_u8(Uu));
			const uint8x8_t Vu = vld1_u8(pV);
			const int16x8_t V = vreinterpretq_s16_u16(vmovl_u8(Vu));
			/* Do the calculations on Y in 32bit width, the result of 255 * 256 does not fit
			 * a signed 16 bit value. */
			const int16x8_t D = vsubq_s16(U, c128);
			const int16x8_t E = vsubq_s16(V, c128);
			pRGB = neon_YuvToRgbPixel(pRGB, Y, D, E, rPos, gPos, bPos, aPos);
			pY += 8;
			pU += 8;
			pV += 8;
		}

		for (x = 0; x < pad; x++)
		{
			const BYTE Y = *pY++;
			const BYTE U = *pU++;
			const BYTE V = *pV++;
			const BYTE r = YUV2R(Y, U, V);
			const BYTE g = YUV2G(Y, U, V);
			const BYTE b = YUV2B(Y, U, V);
			pRGB[aPos] = 0xFF;
			pRGB[rPos] = r;
			pRGB[gPos] = g;
			pRGB[bPos] = b;
			pRGB += 4;
		}

		pRGB += dPad;
		pY += yPad;
		pU += uPad;
		pV += vPad;
	}

	return PRIMITIVES_SUCCESS;
}

static pstatus_t neon_YUV444ToRGB_8u_P3AC4R(const BYTE* const pSrc[3], const UINT32 srcStep[3],
                                            BYTE* pDst, UINT32 dstStep, UINT32 DstFormat,
                                            const prim_size_t* roi)
{
	switch (DstFormat)
	{
		case PIXEL_FORMAT_BGRA32:
		case PIXEL_FORMAT_BGRX32:
			return neon_YUV444ToX(pSrc, srcStep, pDst, dstStep, roi, 2, 1, 0, 3);

		case PIXEL_FORMAT_RGBA32:
		case PIXEL_FORMAT_RGBX32:
			return neon_YUV444ToX(pSrc, srcStep, pDst, dstStep, roi, 0, 1, 2, 3);

		case PIXEL_FORMAT_ARGB32:
		case PIXEL_FORMAT_XRGB32:
			return neon_YUV444ToX(pSrc, srcStep, pDst, dstStep, roi, 1, 2, 3, 0);

		case PIXEL_FORMAT_ABGR32:
		case PIXEL_FORMAT_XBGR32:
			return neon_YUV444ToX(pSrc, srcStep, pDst, dstStep, roi, 3, 2, 1, 0);

		default:
			return generic->YUV444ToRGB_8u_P3AC4R(pSrc, srcStep, pDst, dstStep, DstFormat, roi);
	}
}

static pstatus_t neon_LumaToYUV444(const BYTE* const pSrcRaw[3], const UINT32 srcStep[3],
                                   BYTE* pDstRaw[3], const UINT32 dstStep[3],
                                   const RECTANGLE_16* roi)
{
	UINT32 x, y;
	const UINT32 nWidth = roi->right - roi->left;
	const UINT32 nHeight = roi->bottom - roi->top;
	const UINT32 halfWidth = (nWidth + 1) / 2;
	const UINT32 halfHeight = (nHeight + 1) / 2;
	const UINT32 evenY = 0;
	const BYTE* pSrc[3] = { pSrcRaw[0] + roi->top * srcStep[0] + roi->left,
		                    pSrcRaw[1] + roi->top / 2 * srcStep[1] + roi->left / 2,
		                    pSrcRaw[2] + roi->top / 2 * srcStep[2] + roi->left / 2 };
	BYTE* pDst[3] = { pDstRaw[0] + roi->top * dstStep[0] + roi->left,
		              pDstRaw[1] + roi->top * dstStep[1] + roi->left,
		              pDstRaw[2] + roi->top * dstStep[2] + roi->left };

	/* Y data is already here... */
	/* B1 */
	for (y = 0; y < nHeight; y++)
	{
		const BYTE* Ym = pSrc[0] + srcStep[0] * y;
		BYTE* pY = pDst[0] + dstStep[0] * y;
		memcpy(pY, Ym, nWidth);
	}

	/* The first half of U, V are already here part of this frame. */
	/* B2 and B3 */
	for (y = 0; y < halfHeight; y++)
	{
		const UINT32 val2y = (2 * y + evenY);
		const BYTE* Um = pSrc[1] + srcStep[1] * y;
		const BYTE* Vm = pSrc[2] + srcStep[2] * y;
		BYTE* pU = pDst[1] + dstStep[1] * val2y;
		BYTE* pV = pDst[2] + dstStep[2] * val2y;
		BYTE* pU1 = pU + dstStep[1];
		BYTE* pV1 = pV + dstStep[2];

		for (x = 0; x + 16 < halfWidth; x += 16)
		{
			{
				const uint8x16_t u = vld1q_u8(Um);
				uint8x16x2_t u2x;
				u2x.val[0] = u;
				u2x.val[1] = u;
				vst2q_u8(pU, u2x);
				vst2q_u8(pU1, u2x);
				Um += 16;
				pU += 32;
				pU1 += 32;
			}
			{
				const uint8x16_t v = vld1q_u8(Vm);
				uint8x16x2_t v2x;
				v2x.val[0] = v;
				v2x.val[1] = v;
				vst2q_u8(pV, v2x);
				vst2q_u8(pV1, v2x);
				Vm += 16;
				pV += 32;
				pV1 += 32;
			}
		}

		for (; x < halfWidth; x++)
		{
			const BYTE u = *Um++;
			const BYTE v = *Vm++;
			*pU++ = u;
			*pU++ = u;
			*pU1++ = u;
			*pU1++ = u;
			*pV++ = v;
			*pV++ = v;
			*pV1++ = v;
			*pV1++ = v;
		}
	}

	return PRIMITIVES_SUCCESS;
}

static pstatus_t neon_ChromaFilter(BYTE* pDst[3], const UINT32 dstStep[3], const RECTANGLE_16* roi)
{
	const UINT32 oddY = 1;
	const UINT32 evenY = 0;
	const UINT32 nWidth = roi->right - roi->left;
	const UINT32 nHeight = roi->bottom - roi->top;
	const UINT32 halfHeight = (nHeight + 1) / 2;
	const UINT32 halfWidth = (nWidth + 1) / 2;
	const UINT32 halfPad = halfWidth % 16;
	UINT32 x, y;

	/* Filter */
	for (y = roi->top; y < halfHeight + roi->top; y++)
	{
		const UINT32 val2y = (y * 2 + evenY);
		const UINT32 val2y1 = val2y + oddY;
		BYTE* pU1 = pDst[1] + dstStep[1] * val2y1;
		BYTE* pV1 = pDst[2] + dstStep[2] * val2y1;
		BYTE* pU = pDst[1] + dstStep[1] * val2y;
		BYTE* pV = pDst[2] + dstStep[2] * val2y;

		if (val2y1 > nHeight)
			continue;

		for (x = roi->left / 2; x < halfWidth + roi->left / 2 - halfPad; x += 16)
		{
			{
				/* U = (U2x,2y << 2) - U2x1,2y - U2x,2y1 - U2x1,2y1 */
				uint8x8x2_t u = vld2_u8(&pU[2 * x]);
				const int16x8_t up =
				    vreinterpretq_s16_u16(vshll_n_u8(u.val[0], 2)); /* Ux2,2y << 2 */
				const uint8x8x2_t u1 = vld2_u8(&pU1[2 * x]);
				const uint16x8_t usub = vaddl_u8(u1.val[1], u1.val[0]); /* U2x,2y1 + U2x1,2y1 */
				const int16x8_t us = vreinterpretq_s16_u16(
				    vaddw_u8(usub, u.val[1])); /* U2x1,2y + U2x,2y1 + U2x1,2y1 */
				const int16x8_t un = vsubq_s16(up, us);
				const uint8x8_t u8 = vqmovun_s16(un); /* CLIP(un) */
				u.val[0] = u8;
				vst2_u8(&pU[2 * x], u);
			}
			{
				/* V = (V2x,2y << 2) - V2x1,2y - V2x,2y1 - V2x1,2y1 */
				uint8x8x2_t v = vld2_u8(&pV[2 * x]);
				const int16x8_t vp =
				    vreinterpretq_s16_u16(vshll_n_u8(v.val[0], 2)); /* Vx2,2y << 2 */
				const uint8x8x2_t v1 = vld2_u8(&pV1[2 * x]);
				const uint16x8_t vsub = vaddl_u8(v1.val[1], v1.val[0]); /* V2x,2y1 + V2x1,2y1 */
				const int16x8_t vs = vreinterpretq_s16_u16(
				    vaddw_u8(vsub, v.val[1])); /* V2x1,2y + V2x,2y1 + V2x1,2y1 */
				const int16x8_t vn = vsubq_s16(vp, vs);
				const uint8x8_t v8 = vqmovun_s16(vn); /* CLIP(vn) */
				v.val[0] = v8;
				vst2_u8(&pV[2 * x], v);
			}
		}

		for (; x < halfWidth + roi->left / 2; x++)
		{
			const UINT32 val2x = (x * 2);
			const UINT32 val2x1 = val2x + 1;
			const INT32 up = pU[val2x] * 4;
			const INT32 vp = pV[val2x] * 4;
			INT32 u2020;
			INT32 v2020;

			if (val2x1 > nWidth)
				continue;

			u2020 = up - pU[val2x1] - pU1[val2x] - pU1[val2x1];
			v2020 = vp - pV[val2x1] - pV1[val2x] - pV1[val2x1];
			pU[val2x] = CLIP(u2020);
			pV[val2x] = CLIP(v2020);
		}
	}

	return PRIMITIVES_SUCCESS;
}

static pstatus_t neon_ChromaV1ToYUV444(const BYTE* const pSrcRaw[3], const UINT32 srcStep[3],
                                       BYTE* pDstRaw[3], const UINT32 dstStep[3],
                                       const RECTANGLE_16* roi)
{
	const UINT32 mod = 16;
	UINT32 uY = 0;
	UINT32 vY = 0;
	UINT32 x, y;
	const UINT32 nWidth = roi->right - roi->left;
	const UINT32 nHeight = roi->bottom - roi->top;
	const UINT32 halfWidth = (nWidth) / 2;
	const UINT32 halfHeight = (nHeight) / 2;
	const UINT32 oddY = 1;
	const UINT32 evenY = 0;
	const UINT32 oddX = 1;
	/* The auxilary frame is aligned to multiples of 16x16.
	 * We need the padded height for B4 and B5 conversion. */
	const UINT32 padHeigth = nHeight + 16 - nHeight % 16;
	const UINT32 halfPad = halfWidth % 16;
	const BYTE* pSrc[3] = { pSrcRaw[0] + roi->top * srcStep[0] + roi->left,
		                    pSrcRaw[1] + roi->top / 2 * srcStep[1] + roi->left / 2,
		                    pSrcRaw[2] + roi->top / 2 * srcStep[2] + roi->left / 2 };
	BYTE* pDst[3] = { pDstRaw[0] + roi->top * dstStep[0] + roi->left,
		              pDstRaw[1] + roi->top * dstStep[1] + roi->left,
		              pDstRaw[2] + roi->top * dstStep[2] + roi->left };

	/* The second half of U and V is a bit more tricky... */
	/* B4 and B5 */
	for (y = 0; y < padHeigth; y++)
	{
		const BYTE* Ya = pSrc[0] + srcStep[0] * y;
		BYTE* pX;

		if ((y) % mod < (mod + 1) / 2)
		{
			const UINT32 pos = (2 * uY++ + oddY);

			if (pos >= nHeight)
				continue;

			pX = pDst[1] + dstStep[1] * pos;
		}
		else
		{
			const UINT32 pos = (2 * vY++ + oddY);

			if (pos >= nHeight)
				continue;

			pX = pDst[2] + dstStep[2] * pos;
		}

		memcpy(pX, Ya, nWidth);
	}

	/* B6 and B7 */
	for (y = 0; y < halfHeight; y++)
	{
		const UINT32 val2y = (y * 2 + evenY);
		const BYTE* Ua = pSrc[1] + srcStep[1] * y;
		const BYTE* Va = pSrc[2] + srcStep[2] * y;
		BYTE* pU = pDst[1] + dstStep[1] * val2y;
		BYTE* pV = pDst[2] + dstStep[2] * val2y;

		for (x = 0; x < halfWidth - halfPad; x += 16)
		{
			{
				uint8x16x2_t u = vld2q_u8(&pU[2 * x]);
				u.val[1] = vld1q_u8(&Ua[x]);
				vst2q_u8(&pU[2 * x], u);
			}
			{
				uint8x16x2_t v = vld2q_u8(&pV[2 * x]);
				v.val[1] = vld1q_u8(&Va[x]);
				vst2q_u8(&pV[2 * x], v);
			}
		}

		for (; x < halfWidth; x++)
		{
			const UINT32 val2x1 = (x * 2 + oddX);
			pU[val2x1] = Ua[x];
			pV[val2x1] = Va[x];
		}
	}

	/* Filter */
	return neon_ChromaFilter(pDst, dstStep, roi);
}

static pstatus_t neon_ChromaV2ToYUV444(const BYTE* const pSrc[3], const UINT32 srcStep[3],
                                       UINT32 nTotalWidth, UINT32 nTotalHeight, BYTE* pDst[3],
                                       const UINT32 dstStep[3], const RECTANGLE_16* roi)
{
	UINT32 x, y;
	const UINT32 nWidth = roi->right - roi->left;
	const UINT32 nHeight = roi->bottom - roi->top;
	const UINT32 halfWidth = (nWidth + 1) / 2;
	const UINT32 halfPad = halfWidth % 16;
	const UINT32 halfHeight = (nHeight + 1) / 2;
	const UINT32 quaterWidth = (nWidth + 3) / 4;
	const UINT32 quaterPad = quaterWidth % 16;

	/* B4 and B5: odd UV values for width/2, height */
	for (y = 0; y < nHeight; y++)
	{
		const UINT32 yTop = y + roi->top;
		const BYTE* pYaU = pSrc[0] + srcStep[0] * yTop + roi->left / 2;
		const BYTE* pYaV = pYaU + nTotalWidth / 2;
		BYTE* pU = pDst[1] + dstStep[1] * yTop + roi->left;
		BYTE* pV = pDst[2] + dstStep[2] * yTop + roi->left;

		for (x = 0; x < halfWidth - halfPad; x += 16)
		{
			{
				uint8x16x2_t u = vld2q_u8(&pU[2 * x]);
				u.val[1] = vld1q_u8(&pYaU[x]);
				vst2q_u8(&pU[2 * x], u);
			}
			{
				uint8x16x2_t v = vld2q_u8(&pV[2 * x]);
				v.val[1] = vld1q_u8(&pYaV[x]);
				vst2q_u8(&pV[2 * x], v);
			}
		}

		for (; x < halfWidth; x++)
		{
			const UINT32 odd = 2 * x + 1;
			pU[odd] = pYaU[x];
			pV[odd] = pYaV[x];
		}
	}

	/* B6 - B9 */
	for (y = 0; y < halfHeight; y++)
	{
		const BYTE* pUaU = pSrc[1] + srcStep[1] * (y + roi->top / 2) + roi->left / 4;
		const BYTE* pUaV = pUaU + nTotalWidth / 4;
		const BYTE* pVaU = pSrc[2] + srcStep[2] * (y + roi->top / 2) + roi->left / 4;
		const BYTE* pVaV = pVaU + nTotalWidth / 4;
		BYTE* pU = pDst[1] + dstStep[1] * (2 * y + 1 + roi->top) + roi->left;
		BYTE* pV = pDst[2] + dstStep[2] * (2 * y + 1 + roi->top) + roi->left;

		for (x = 0; x < quaterWidth - quaterPad; x += 16)
		{
			{
				uint8x16x4_t u = vld4q_u8(&pU[4 * x]);
				u.val[0] = vld1q_u8(&pUaU[x]);
				u.val[2] = vld1q_u8(&pVaU[x]);
				vst4q_u8(&pU[4 * x], u);
			}
			{
				uint8x16x4_t v = vld4q_u8(&pV[4 * x]);
				v.val[0] = vld1q_u8(&pUaV[x]);
				v.val[2] = vld1q_u8(&pVaV[x]);
				vst4q_u8(&pV[4 * x], v);
			}
		}

		for (; x < quaterWidth; x++)
		{
			pU[4 * x + 0] = pUaU[x];
			pV[4 * x + 0] = pUaV[x];
			pU[4 * x + 2] = pVaU[x];
			pV[4 * x + 2] = pVaV[x];
		}
	}

	return neon_ChromaFilter(pDst, dstStep, roi);
}

static pstatus_t neon_YUV420CombineToYUV444(avc444_frame_type type, const BYTE* const pSrc[3],
                                            const UINT32 srcStep[3], UINT32 nWidth, UINT32 nHeight,
                                            BYTE* pDst[3], const UINT32 dstStep[3],
                                            const RECTANGLE_16* roi)
{
	if (!pSrc || !pSrc[0] || !pSrc[1] || !pSrc[2])
		return -1;

	if (!pDst || !pDst[0] || !pDst[1] || !pDst[2])
		return -1;

	if (!roi)
		return -1;

	switch (type)
	{
		case AVC444_LUMA:
			return neon_LumaToYUV444(pSrc, srcStep, pDst, dstStep, roi);

		case AVC444_CHROMAv1:
			return neon_ChromaV1ToYUV444(pSrc, srcStep, pDst, dstStep, roi);

		case AVC444_CHROMAv2:
			return neon_ChromaV2ToYUV444(pSrc, srcStep, nWidth, nHeight, pDst, dstStep, roi);

		default:
			return -1;
	}
}

void primitives_init_YUV_opt(primitives_t* prims)
{
	generic = primitives_get_generic();
	primitives_init_YUV(prims);

	if (IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE))
	{
		prims->YUV420ToRGB_8u_P3AC4R = neon_YUV420ToRGB_8u_P3AC4R;
		prims->YUV444ToRGB_8u_P3AC4R = neon_YUV444ToRGB_8u_P3AC4R;
		prims->YUV420CombineToYUV444 = neon_YUV420CombineToYUV444;
	}
}
