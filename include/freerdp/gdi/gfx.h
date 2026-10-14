/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * GDI Graphics Pipeline
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#ifndef FREERDP_GDI_GFX_H
#define FREERDP_GDI_GFX_H

#include <freerdp/api.h>
#include <freerdp/gdi/gdi.h>

struct gdi_gfx_surface
{
	UINT16 surfaceId;
	rdpCodecs* codecs;
	H264_CONTEXT* h264;
	UINT32 width;
	UINT32 height;
	UINT32 mappedWidth;
	UINT32 mappedHeight;
	BYTE* data;
	UINT32 scanline;
	UINT32 format;
	BOOL outputMapped;
	UINT32 outputOriginX;
	UINT32 outputOriginY;
	REGION16 invalidRegion;
	UINT64 windowId;
	UINT32 outputTargetWidth;
	UINT32 outputTargetHeight;
};
typedef struct gdi_gfx_surface gdiGfxSurface;

struct gdi_gfx_cache_entry
{
	UINT64 cacheKey;
	UINT32 width;
	UINT32 height;
	BYTE* data;
	UINT32 scanline;
	UINT32 format;
};
typedef struct gdi_gfx_cache_entry gdiGfxCacheEntry;

#ifdef __cplusplus
extern "C"
{
#endif

	FREERDP_API BOOL gdi_graphics_pipeline_init(rdpGdi* gdi, RdpgfxClientContext* gfx);
	FREERDP_API BOOL gdi_graphics_pipeline_init_ex(rdpGdi* gdi, RdpgfxClientContext* gfx,
	                                               pcRdpgfxMapWindowForSurface map,
	                                               pcRdpgfxUnmapWindowForSurface unmap,
	                                               pcRdpgfxUpdateSurfaceArea update);
	FREERDP_API void gdi_graphics_pipeline_uninit(rdpGdi* gdi, RdpgfxClientContext* gfx);

	/* H.264 解码领先一帧的尾帧补吐(见 libfreerdp/codec/h264.c 的 avc420_flush_pending):
	   最后收到的那帧没有下一帧来触发转换,客户端主循环需要主动把它吐出来,否则画面会
	   一直停在倒数第二帧。frame_pending 用来决定要不要把等待超时改短。 */
	FREERDP_API BOOL gdi_gfx_frame_pending(RdpgfxClientContext* context);
	FREERDP_API UINT gdi_gfx_flush_frame(RdpgfxClientContext* context);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_GDI_GFX_H */
