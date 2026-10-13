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
