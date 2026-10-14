/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * RemoteFX Codec Library - Decode
 *
 * Copyright 2018 Armin Novak <anovak@thincast.com>
 * Copyright 2018 Thincast Technologies GmbH
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

#ifndef FREERDP_LIB_CODEC_H264_H
#define FREERDP_LIB_CODEC_H264_H

#include <freerdp/api.h>
#include <freerdp/codec/h264.h>

FREERDP_LOCAL BOOL avc420_ensure_buffer(H264_CONTEXT* h264, UINT32 stride, UINT32 width,
                                        UINT32 height);

/* 清空领先一帧的在途状态(尺寸变化/解码器重建时调用) */
FREERDP_LOCAL void h264_lookahead_reset(H264_CONTEXT* h264);

/* 把在途的那帧吐出来并转换(只取不喂)。返回是否真的转换出了一帧。 */
FREERDP_LOCAL BOOL avc420_flush_pending(H264_CONTEXT* h264, BYTE* pDstData, DWORD DstFormat,
                                        UINT32 nDstStep, UINT32 nDstWidth, UINT32 nDstHeight);

#endif /* FREERDP_LIB_CODEC_H264_H */
