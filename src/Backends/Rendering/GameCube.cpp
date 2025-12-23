// Released under the MIT licence.
// See LICENCE.txt for details.

// GameCube GX Hardware Renderer for CSE2

#include "../Rendering.h"
#include "../Misc.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

#include <gccore.h>
#include <ogc/gx.h>

#define GC_LOG(fmt, ...) printf("[GX Render] " fmt "\n", ##__VA_ARGS__)

// Game resolution - 320x240, GX hardware scales to 640x480 output
#define GAME_WIDTH 320
#define GAME_HEIGHT 240

// GX texture format - use RGB5A3 for color key support
// RGB5A3: if MSB=1: RGB555, if MSB=0: ARGB3444
#define TEXTURE_FORMAT GX_TF_RGB5A3

// Texture cache invalidation tracking - avoids redundant invalidations
static bool textures_need_invalidation = false;

// Helper to mark textures dirty (deferred invalidation)
static inline void MarkTexturesDirty(void)
{
	textures_need_invalidation = true;
}

// Helper to sync GPU and invalidate texture cache if needed
static inline void SyncAndInvalidateTextures(void)
{
	if (textures_need_invalidation)
	{
		// Wait for all pending GX commands to complete
		GX_DrawDone();
		// Invalidate texture cache
		GX_InvalidateTexAll();
		// IMPORTANT: Wait again to ensure invalidation completes before next texture load
		// Without this, GX_LoadTexObj() might race with the invalidation command
		GX_DrawDone();
		textures_need_invalidation = false;
	}
}

// Surface structure - holds GX texture
struct RenderBackend_Surface {
	GXTexObj texObj;
	void *texture_data;  // 32-byte aligned texture data
	size_t width;
	size_t height;
	size_t tex_width;    // Power of 2 width
	size_t tex_height;   // Power of 2 height
	bool has_colorkey;   // Has transparent pixels
	bool is_hires;       // Hi-res text surface (uses 640x480 projection when blitting)
};

// Glyph atlas
struct RenderBackend_GlyphAtlas {
	GXTexObj texObj;
	void *texture_data;
	size_t width;
	size_t height;
	size_t tex_width;
	size_t tex_height;
};

// Video globals
static GXRModeObj *vmode = NULL;
static void *xfb[2] = {NULL, NULL};
static int whichfb = 0;
static void *gp_fifo = NULL;

// Framebuffer surface (represents screen)
static RenderBackend_Surface *framebuffer_surface = NULL;

// Current render target
static RenderBackend_Surface *current_target = NULL;

// Glyph rendering state
static RenderBackend_GlyphAtlas *glyph_atlas = NULL;
static RenderBackend_Surface *glyph_dest = NULL;
static u8 glyph_r, glyph_g, glyph_b;

// Hi-res text rendering state (640x480 for text while game is 320x240)
static bool in_hires_text_mode = false;
static void SetupHiResTextProjection(void);
static void RestoreNormalProjection(void);

// Debug
static int frame_count = 0;

// Round up to next power of 2
static size_t NextPow2(size_t n)
{
	n--;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	n |= n >> 16;
	n++;
	return n < 8 ? 8 : n;  // Minimum 8 for GX
}

// Convert RGB to RGB5A3 (with alpha for color key)
static inline u16 RGB_to_RGB5A3(u8 r, u8 g, u8 b, bool transparent)
{
	if (transparent)
	{
		// ARGB3444 format (MSB=0)
		return ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
	}
	else
	{
		// RGB555 format (MSB=1)
		return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
	}
}

// Setup GX for 2D rendering
static void SetupGX(void)
{
	Mtx44 ortho;
	Mtx identity;
	
	// Set viewport
	GX_SetViewport(0, 0, vmode->fbWidth, vmode->efbHeight, 0, 1);
	
	// Set scissor
	GX_SetScissor(0, 0, vmode->fbWidth, vmode->efbHeight);
	
	// Orthographic projection for 2D
	guOrtho(ortho, 0, GAME_HEIGHT, 0, GAME_WIDTH, 0, 1);
	GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
	
	// Identity modelview
	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	
	// Vertex format: position (x,y) + texcoord (s,t)
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	
	// TEV setup for textured quads
	GX_SetNumChans(0);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	
	// Alpha test for color key (discard pixels with alpha < 0.5)
	GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZCompLoc(GX_TRUE);
	
	// Blending
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	
	// No Z buffer for 2D
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	
	// Cull none
	GX_SetCullMode(GX_CULL_NONE);
}

// Setup TEV for color fill
static void SetupTEVColorFill(u8 r, u8 g, u8 b)
{
	(void)r; (void)g; (void)b;  // Color passed via vertex data
	GX_SetNumChans(1);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
}

// Restore TEV for textured drawing
static void SetupTEVTextured(void)
{
	GX_SetNumChans(0);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
}

RenderBackend_Surface* RenderBackend_Init(const char *window_title, size_t width, size_t height, bool fullscreen)
{
	(void)window_title;
	(void)fullscreen;
	(void)width;
	(void)height;
	
	GC_LOG("=== GX Hardware Renderer Init ===");
	
	// Get video mode
	vmode = VIDEO_GetPreferredMode(NULL);
	if (!vmode)
	{
		GC_LOG("ERROR: No video mode!");
		return NULL;
	}
	GC_LOG("Video: %dx%d", vmode->fbWidth, vmode->xfbHeight);
	
	// Allocate XFB
	xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(vmode));
	xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(vmode));
	if (!xfb[0] || !xfb[1])
	{
		GC_LOG("ERROR: XFB alloc failed!");
		return NULL;
	}
	GC_LOG("XFB: %p, %p", xfb[0], xfb[1]);
	
	// Clear XFB
	VIDEO_ClearFrameBuffer(vmode, xfb[0], COLOR_BLACK);
	VIDEO_ClearFrameBuffer(vmode, xfb[1], COLOR_BLACK);
	
	// Configure video
	VIDEO_Configure(vmode);
	VIDEO_SetNextFramebuffer(xfb[0]);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (vmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	
	// Initialize GX
	gp_fifo = memalign(32, 256 * 1024);  // 256KB FIFO
	if (!gp_fifo)
	{
		GC_LOG("ERROR: FIFO alloc failed!");
		return NULL;
	}
	memset(gp_fifo, 0, 256 * 1024);
	GX_Init(gp_fifo, 256 * 1024);
	GC_LOG("GX initialized");
	
	// Set background color (black)
	GXColor bg = {0, 0, 0, 0xFF};
	GX_SetCopyClear(bg, GX_MAX_Z24);
	
	// Set display copy parameters
	GX_SetDispCopyGamma(GX_GM_1_0);
	f32 yscale = GX_GetYScaleFactor(vmode->efbHeight, vmode->xfbHeight);
	u32 xfbHeight = GX_SetDispCopyYScale(yscale);
	GX_SetDispCopySrc(0, 0, vmode->fbWidth, vmode->efbHeight);
	GX_SetDispCopyDst(vmode->fbWidth, xfbHeight);
	GX_SetCopyFilter(vmode->aa, vmode->sample_pattern, GX_TRUE, vmode->vfilter);
	GX_SetFieldMode(vmode->field_rendering, ((vmode->viHeight == 2 * vmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
	
	if (vmode->aa)
		GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
	else
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	
	// Setup for 2D rendering
	SetupGX();
	
	// Create framebuffer surface struct (represents the screen)
	framebuffer_surface = (RenderBackend_Surface*)malloc(sizeof(RenderBackend_Surface));
	if (!framebuffer_surface)
	{
		GC_LOG("ERROR: fb surface alloc failed!");
		return NULL;
	}
	memset(framebuffer_surface, 0, sizeof(RenderBackend_Surface));
	framebuffer_surface->width = GAME_WIDTH;
	framebuffer_surface->height = GAME_HEIGHT;
	
	current_target = framebuffer_surface;
	
	GC_LOG("Init complete!");
	return framebuffer_surface;
}

void RenderBackend_Deinit(void)
{
	if (framebuffer_surface)
	{
		free(framebuffer_surface);
		framebuffer_surface = NULL;
	}
	if (gp_fifo)
	{
		free(gp_fifo);
		gp_fifo = NULL;
	}
}

void RenderBackend_DrawScreen(void)
{
	frame_count++;
	
	// Restore normal projection for next frame
	RestoreNormalProjection();
	
	// Ensure any pending texture invalidations are processed
	SyncAndInvalidateTextures();
	
	// Finish rendering
	GX_DrawDone();
	
	// Copy EFB to XFB
	GX_CopyDisp(xfb[whichfb], GX_TRUE);
	GX_Flush();
	
	// Flip
	VIDEO_SetNextFramebuffer(xfb[whichfb]);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	whichfb ^= 1;
	
	// Clear for next frame
	GXColor bg = {0, 0, 0, 0xFF};
	GX_SetCopyClear(bg, GX_MAX_Z24);
	
	// Reset vertex cache at frame boundary (devkitpro examples do this)
	GX_InvVtxCache();
	
	if (frame_count <= 5 || frame_count % 60 == 0)
		GC_LOG("Frame %d", frame_count);
}

RenderBackend_Surface* RenderBackend_CreateSurface(size_t width, size_t height, bool render_target)
{
	(void)render_target;
	
	RenderBackend_Surface *surface = (RenderBackend_Surface*)malloc(sizeof(RenderBackend_Surface));
	if (!surface) return NULL;
	
	memset(surface, 0, sizeof(RenderBackend_Surface));
	surface->width = width;
	surface->height = height;
	surface->tex_width = NextPow2(width);
	surface->tex_height = NextPow2(height);
	
	// Allocate texture data (RGB5A3 = 2 bytes per pixel, 32-byte aligned)
	size_t tex_size = surface->tex_width * surface->tex_height * 2;
	surface->texture_data = memalign(32, tex_size);
	if (!surface->texture_data)
	{
		GC_LOG("CreateSurface FAILED: %zux%zu", width, height);
		free(surface);
		return NULL;
	}
	memset(surface->texture_data, 0, tex_size);
	
	// Initialize texture object
	GX_InitTexObj(&surface->texObj, surface->texture_data, 
		surface->tex_width, surface->tex_height, 
		TEXTURE_FORMAT, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&surface->texObj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	
	GC_LOG("CreateSurface: %zux%zu (tex: %zux%zu)", width, height, surface->tex_width, surface->tex_height);
	return surface;
}

void RenderBackend_FreeSurface(RenderBackend_Surface *surface)
{
	if (surface && surface != framebuffer_surface)
	{
		if (surface->texture_data)
			free(surface->texture_data);
		free(surface);
	}
}

bool RenderBackend_IsSurfaceLost(RenderBackend_Surface *surface) { (void)surface; return false; }
void RenderBackend_RestoreSurface(RenderBackend_Surface *surface) { (void)surface; }

// Mark a surface as hi-res (for 640x480 text rendering)
void RenderBackend_SetSurfaceHiRes(RenderBackend_Surface *surface, bool hires)
{
	if (surface && surface != framebuffer_surface)
	{
		surface->is_hires = hires;
		if (hires)
			GC_LOG("Surface marked as hi-res: %zux%zu", surface->width, surface->height);
	}
}

void RenderBackend_UploadSurface(RenderBackend_Surface *surface, const unsigned char *pixels, size_t width, size_t height)
{
	if (!surface || !pixels || surface == framebuffer_surface) return;
	
	// Ensure GPU is done reading any textures before we modify them
	GX_DrawDone();
	
	// Reallocate if size changed
	if (surface->width != width || surface->height != height)
	{
		surface->width = width;
		surface->height = height;
		surface->tex_width = NextPow2(width);
		surface->tex_height = NextPow2(height);
		
		if (surface->texture_data)
			free(surface->texture_data);
		
		size_t tex_size = surface->tex_width * surface->tex_height * 2;
		surface->texture_data = memalign(32, tex_size);
		if (!surface->texture_data) return;
		memset(surface->texture_data, 0, tex_size);
		
		GX_InitTexObj(&surface->texObj, surface->texture_data,
			surface->tex_width, surface->tex_height,
			TEXTURE_FORMAT, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjLOD(&surface->texObj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	}
	
	// Convert RGB to RGB5A3 with tiled layout
	// GX textures use 4x4 tile format for RGB5A3
	u16 *tex = (u16*)surface->texture_data;
	
	for (size_t ty = 0; ty < surface->tex_height; ty += 4)
	{
		for (size_t tx = 0; tx < surface->tex_width; tx += 4)
		{
			for (size_t y = 0; y < 4; y++)
			{
				for (size_t x = 0; x < 4; x++)
				{
					size_t px = tx + x;
					size_t py = ty + y;
					
					u16 pixel;
					if (px < width && py < height)
					{
						size_t src_idx = (py * width + px) * 3;
						u8 r = pixels[src_idx + 0];
						u8 g = pixels[src_idx + 1];
						u8 b = pixels[src_idx + 2];
						
						// Color key: black (0,0,0) is transparent
						bool transparent = (r == 0 && g == 0 && b == 0);
						if (transparent)
							surface->has_colorkey = true;
						
						pixel = RGB_to_RGB5A3(r, g, b, transparent);
					}
					else
					{
						pixel = 0;  // Transparent padding
					}
					
					*tex++ = pixel;
				}
			}
		}
	}
	
	// Flush texture to main memory and mark for cache invalidation
	DCFlushRange(surface->texture_data, surface->tex_width * surface->tex_height * 2);
	MarkTexturesDirty();
}

// Helper to get pixel from tiled texture
static inline u16 GetTiledPixel(u16 *tex, size_t tex_width, size_t px, size_t py)
{
	size_t tile_x = px / 4;
	size_t tile_y = py / 4;
	size_t in_tile_x = px % 4;
	size_t in_tile_y = py % 4;
	size_t tiles_per_row = tex_width / 4;
	size_t tile_idx = tile_y * tiles_per_row + tile_x;
	size_t pixel_in_tile = in_tile_y * 4 + in_tile_x;
	return tex[tile_idx * 16 + pixel_in_tile];
}

// Helper to set pixel in tiled texture
static inline void SetTiledPixel(u16 *tex, size_t tex_width, size_t px, size_t py, u16 pixel)
{
	size_t tile_x = px / 4;
	size_t tile_y = py / 4;
	size_t in_tile_x = px % 4;
	size_t in_tile_y = py % 4;
	size_t tiles_per_row = tex_width / 4;
	size_t tile_idx = tile_y * tiles_per_row + tile_x;
	size_t pixel_in_tile = in_tile_y * 4 + in_tile_x;
	tex[tile_idx * 16 + pixel_in_tile] = pixel;
}

void RenderBackend_Blit(RenderBackend_Surface *source_surface, const RenderBackend_Rect *rect, RenderBackend_Surface *destination_surface, long x, long y, bool colour_key)
{
	if (!source_surface || !rect) return;
	
	// Special case: Copy from framebuffer (EFB) to a surface texture
	// This is used by BackupSurface to capture the screen
	if (source_surface == framebuffer_surface)
	{
		if (!destination_surface || !destination_surface->texture_data) return;
		
		// Ensure all pending GX commands are complete before EFB copy
		GX_DrawDone();
		
		// EFB dimensions (640x480, game renders at 320x240 but GX scales to EFB)
		// Source region in EFB coordinates (multiply by 2 since game coords are 320x240)
		u16 efb_x = (u16)(rect->left * 2);
		u16 efb_y = (u16)(rect->top * 2);
		u16 efb_w = (u16)((rect->right - rect->left) * 2);
		u16 efb_h = (u16)((rect->bottom - rect->top) * 2);
		
		// EFB copy requires coordinates to be multiples of 2
		efb_x &= ~1;
		efb_y &= ~1;
		efb_w = (efb_w + 1) & ~1;
		efb_h = (efb_h + 1) & ~1;
		
		// Clamp to EFB bounds
		if (efb_x + efb_w > 640) efb_w = 640 - efb_x;
		if (efb_y + efb_h > 480) efb_h = 480 - efb_y;
		
		// Texture copy destination dimensions - must copy at EFB size, then downsample on CPU
		// Use power of 2 dimensions for the temp texture
		size_t temp_tex_w = NextPow2(efb_w);
		size_t temp_tex_h = NextPow2(efb_h);
		
		// Allocate temporary buffer for EFB copy (must be 32-byte aligned)
		// Using RGBA8 format (4 bytes per pixel) which is more compatible
		size_t temp_size = temp_tex_w * temp_tex_h * 4;  // RGBA8 = 4 bytes per pixel
		void *temp_tex = memalign(32, temp_size);
		if (!temp_tex) 
		{
			GC_LOG("BackupSurface: Failed to allocate temp buffer (%zu bytes)", temp_size);
			return;
		}
		memset(temp_tex, 0, temp_size);
		
		// Invalidate cache range before GX writes to it
		DCInvalidateRange(temp_tex, temp_size);
		
		// Setup EFB copy source region
		GX_SetTexCopySrc(efb_x, efb_y, efb_w, efb_h);
		
		// Setup EFB copy destination - RGBA8 format is more reliable
		GX_SetTexCopyDst(temp_tex_w, temp_tex_h, GX_TF_RGBA8, GX_FALSE);
		
		// Perform the EFB to texture copy
		GX_CopyTex(temp_tex, GX_FALSE);  // GX_FALSE = don't clear EFB
		
		// Synchronize - wait for copy to complete before CPU access
		GX_PixModeSync();
		GX_DrawDone();
		
		// Invalidate cache so CPU sees the GPU's writes
		DCInvalidateRange(temp_tex, temp_size);
		
		// Now downsample from temp texture (RGBA8 tiled) to destination surface (RGB5A3 tiled)
		// RGBA8 uses 4x4 tiles with 64 bytes per tile (4 bytes per pixel, AR then GB pairs)
		u8 *src_tex = (u8*)temp_tex;
		u16 *dst_tex = (u16*)destination_surface->texture_data;
		
		// Destination dimensions (in game coordinates, half of EFB)
		size_t dst_w = efb_w / 2;
		size_t dst_h = efb_h / 2;
		
		for (size_t py = 0; py < dst_h; py++)
		{
			for (size_t px = 0; px < dst_w; px++)
			{
				size_t dx = x + px;
				size_t dy = y + py;
				
				if (dx >= destination_surface->width || dy >= destination_surface->height)
					continue;
				
				// Sample from EFB at 2x coordinates (simple point sample, could do box filter)
				size_t sx = px * 2;
				size_t sy = py * 2;
				
				// Read RGBA8 pixel from tiled format
				// RGBA8 tile layout: 4x4 tiles, each tile has AR pairs then GB pairs
				size_t tile_x = sx / 4;
				size_t tile_y = sy / 4;
				size_t in_tile_x = sx % 4;
				size_t in_tile_y = sy % 4;
				size_t tiles_per_row = temp_tex_w / 4;
				size_t tile_idx = tile_y * tiles_per_row + tile_x;
				size_t pixel_in_tile = in_tile_y * 4 + in_tile_x;
				
				// RGBA8 tile: first 32 bytes are AR pairs, next 32 bytes are GB pairs
				size_t ar_offset = tile_idx * 64 + pixel_in_tile * 2;
				size_t gb_offset = tile_idx * 64 + 32 + pixel_in_tile * 2;
				
				u8 a = src_tex[ar_offset];
				u8 r = src_tex[ar_offset + 1];
				u8 g = src_tex[gb_offset];
				u8 b = src_tex[gb_offset + 1];
				
				// Convert to RGB5A3
				u16 pixel;
				if (a < 224)
				{
					// Use ARGB3444 format (has alpha)
					pixel = ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
				}
				else
				{
					// Use RGB555 format (opaque)
					pixel = 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
				}
				
				SetTiledPixel(dst_tex, destination_surface->tex_width, dx, dy, pixel);
			}
		}
		
		free(temp_tex);
		
		// Flush destination texture and mark for cache invalidation
		DCFlushRange(destination_surface->texture_data, destination_surface->tex_width * destination_surface->tex_height * 2);
		MarkTexturesDirty();
		
		GC_LOG("BackupSurface: Copied EFB (%d,%d %dx%d) -> surface at (%ld,%ld)", 
		       efb_x, efb_y, efb_w, efb_h, x, y);
		return;
	}
	
	// Regular surface-to-surface or surface-to-framebuffer blit requires source texture data
	if (!source_surface->texture_data) return;
	
	// Draw to framebuffer using GX
	if (destination_surface == framebuffer_surface)
	{
		// Hi-res surfaces (text lines) use 640x480 projection for sharp rendering
		if (source_surface->is_hires)
		{
			SetupHiResTextProjection();
		}
		else
		{
			// Restore normal projection for regular sprites
			RestoreNormalProjection();
		}
		
		// Sync and invalidate textures before drawing if any were modified
		SyncAndInvalidateTextures();
		
		float tex_w = (float)source_surface->tex_width;
		float tex_h = (float)source_surface->tex_height;
		
		float s0 = (float)rect->left / tex_w;
		float t0 = (float)rect->top / tex_h;
		float s1 = (float)rect->right / tex_w;
		float t1 = (float)rect->bottom / tex_h;
		
		// For hi-res surfaces, scale destination coordinates to 640x480
		float scale = source_surface->is_hires ? 2.0f : 1.0f;
		float x0 = (float)x * scale;
		float y0 = (float)y * scale;
		float x1 = x0 + (rect->right - rect->left);  // Already scaled in source rect
		float y1 = y0 + (rect->bottom - rect->top);
		
		SetupTEVTextured();
		
		if (colour_key && source_surface->has_colorkey)
			GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
		else
			GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
		
		GX_LoadTexObj(&source_surface->texObj, GX_TEXMAP0);
		
		GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
			GX_Position2f32(x0, y0);
			GX_TexCoord2f32(s0, t0);
			GX_Position2f32(x1, y0);
			GX_TexCoord2f32(s1, t0);
			GX_Position2f32(x1, y1);
			GX_TexCoord2f32(s1, t1);
			GX_Position2f32(x0, y1);
			GX_TexCoord2f32(s0, t1);
		GX_End();
		GX_DrawDone();  // Force complete sync - diagnostic for tile corruption
		return;
	}
	
	// For other surfaces, copy pixel data directly
	if (!destination_surface || !destination_surface->texture_data) return;
	
	// Ensure GPU is done before we modify the destination texture
	GX_DrawDone();
	
	u16 *src_tex = (u16*)source_surface->texture_data;
	u16 *dst_tex = (u16*)destination_surface->texture_data;
	
	long src_x = rect->left;
	long src_y = rect->top;
	long width = rect->right - rect->left;
	long height = rect->bottom - rect->top;
	
	// Check if we need to scale up: normal-res source to hi-res destination
	bool scale_up = destination_surface->is_hires && !source_surface->is_hires;
	
	for (long py = 0; py < height; py++)
	{
		for (long px = 0; px < width; px++)
		{
			long sx = src_x + px;
			long sy = src_y + py;
			
			// Bounds check source
			if (sx < 0 || sy < 0 || (size_t)sx >= source_surface->tex_width || (size_t)sy >= source_surface->tex_height)
				continue;
			
			u16 pixel = GetTiledPixel(src_tex, source_surface->tex_width, sx, sy);
			
			// Skip transparent pixels if color key enabled
			if (colour_key)
			{
				// RGB5A3: bit 15 = 0 means has alpha, check if alpha is 0
				if ((pixel & 0x8000) == 0 && (pixel & 0x7000) == 0)
					continue;  // Fully transparent
			}
			
			if (scale_up)
			{
				// Scale up 2x: each source pixel becomes a 2x2 block in destination
				long dx_base = x + px * 2;
				long dy_base = y + py * 2;
				
				for (int sy2 = 0; sy2 < 2; sy2++)
				{
					for (int sx2 = 0; sx2 < 2; sx2++)
					{
						long dx = dx_base + sx2;
						long dy = dy_base + sy2;
						
						if (dx < 0 || dy < 0 || (size_t)dx >= destination_surface->width || (size_t)dy >= destination_surface->height)
							continue;
						
						SetTiledPixel(dst_tex, destination_surface->tex_width, dx, dy, pixel);
					}
				}
			}
			else
			{
				// Normal 1:1 copy
				long dx = x + px;
				long dy = y + py;
				
				if (dx < 0 || dy < 0 || (size_t)dx >= destination_surface->width || (size_t)dy >= destination_surface->height)
					continue;
				
				SetTiledPixel(dst_tex, destination_surface->tex_width, dx, dy, pixel);
			}
		}
	}
	
	// Mark destination as having color key if source does
	if (source_surface->has_colorkey)
		destination_surface->has_colorkey = true;
	
	// Flush texture to main memory and mark for cache invalidation
	DCFlushRange(destination_surface->texture_data, destination_surface->tex_width * destination_surface->tex_height * 2);
	MarkTexturesDirty();
}

void RenderBackend_ColourFill(RenderBackend_Surface *surface, const RenderBackend_Rect *rect, unsigned char red, unsigned char green, unsigned char blue)
{
	if (!rect) return;
	
	// Draw to framebuffer using GX
	if (surface == framebuffer_surface)
	{
		// Restore normal projection if we were in hi-res text mode
		RestoreNormalProjection();
		
		// Sync textures before drawing
		SyncAndInvalidateTextures();
		float x0 = (float)rect->left;
		float y0 = (float)rect->top;
		float x1 = (float)rect->right;
		float y1 = (float)rect->bottom;
		
		SetupTEVColorFill(red, green, blue);
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
		
		GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
			GX_Position2f32(x0, y0);
			GX_Color4u8(red, green, blue, 0xFF);
			GX_Position2f32(x1, y0);
			GX_Color4u8(red, green, blue, 0xFF);
			GX_Position2f32(x1, y1);
			GX_Color4u8(red, green, blue, 0xFF);
			GX_Position2f32(x0, y1);
			GX_Color4u8(red, green, blue, 0xFF);
		GX_End();
		GX_DrawDone();  // Force complete sync - diagnostic for tile corruption
		return;
	}
	
	// For other surfaces, modify texture data directly
	if (!surface || !surface->texture_data) return;
	
	// Ensure GPU is done before we modify the texture
	GX_DrawDone();
	
	// Convert color to RGB5A3
	bool transparent = (red == 0 && green == 0 && blue == 0);
	u16 pixel = RGB_to_RGB5A3(red, green, blue, transparent);
	if (transparent) surface->has_colorkey = true;
	
	// Fill rect in tiled texture format
	size_t x0 = rect->left;
	size_t y0 = rect->top;
	size_t x1 = rect->right;
	size_t y1 = rect->bottom;
	
	// Clamp to surface bounds
	if (x1 > surface->width) x1 = surface->width;
	if (y1 > surface->height) y1 = surface->height;
	
	u16 *tex = (u16*)surface->texture_data;
	
	for (size_t py = y0; py < y1; py++)
	{
		for (size_t px = x0; px < x1; px++)
		{
			// Calculate tiled texture offset (4x4 tiles)
			size_t tile_x = px / 4;
			size_t tile_y = py / 4;
			size_t in_tile_x = px % 4;
			size_t in_tile_y = py % 4;
			
			size_t tiles_per_row = surface->tex_width / 4;
			size_t tile_idx = tile_y * tiles_per_row + tile_x;
			size_t pixel_in_tile = in_tile_y * 4 + in_tile_x;
			size_t tex_idx = tile_idx * 16 + pixel_in_tile;
			
			tex[tex_idx] = pixel;
		}
	}
	
	// Flush texture to main memory and mark for cache invalidation
	DCFlushRange(surface->texture_data, surface->tex_width * surface->tex_height * 2);
	MarkTexturesDirty();
}

RenderBackend_GlyphAtlas* RenderBackend_CreateGlyphAtlas(size_t width, size_t height)
{
	RenderBackend_GlyphAtlas *atlas = (RenderBackend_GlyphAtlas*)malloc(sizeof(RenderBackend_GlyphAtlas));
	if (!atlas) return NULL;
	
	memset(atlas, 0, sizeof(RenderBackend_GlyphAtlas));
	atlas->width = width;
	atlas->height = height;
	atlas->tex_width = NextPow2(width);
	atlas->tex_height = NextPow2(height);
	
	// Use IA8 format (intensity + alpha, 2 bytes per pixel, 4x4 tiles)
	size_t tex_size = atlas->tex_width * atlas->tex_height * 2;
	atlas->texture_data = memalign(32, tex_size);
	if (!atlas->texture_data)
	{
		free(atlas);
		return NULL;
	}
	memset(atlas->texture_data, 0, tex_size);
	
	GX_InitTexObj(&atlas->texObj, atlas->texture_data,
		atlas->tex_width, atlas->tex_height,
		GX_TF_IA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&atlas->texObj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	
	GC_LOG("CreateGlyphAtlas: %zux%zu (IA8)", width, height);
	return atlas;
}

void RenderBackend_DestroyGlyphAtlas(RenderBackend_GlyphAtlas *atlas)
{
	if (atlas)
	{
		if (atlas->texture_data)
			free(atlas->texture_data);
		free(atlas);
	}
}

void RenderBackend_UploadGlyph(RenderBackend_GlyphAtlas *atlas, size_t x, size_t y, const unsigned char *pixels, size_t width, size_t height, size_t pitch)
{
	if (!atlas || !atlas->texture_data || !pixels) return;
	
	// Ensure GPU is done reading the atlas before we modify it
	GX_DrawDone();
	
	// IA8 uses 4x4 tiles, 2 bytes per pixel (alpha, intensity)
	u8 *tex = (u8*)atlas->texture_data;
	
	for (size_t gy = 0; gy < height; gy++)
	{
		for (size_t gx = 0; gx < width; gx++)
		{
			size_t dx = x + gx;
			size_t dy = y + gy;
			
			if (dx >= atlas->tex_width || dy >= atlas->tex_height) continue;
			
			// Calculate tiled offset for IA8 (4x4 tiles, 32 bytes per tile)
			size_t tile_x = dx / 4;
			size_t tile_y = dy / 4;
			size_t in_tile_x = dx % 4;
			size_t in_tile_y = dy % 4;
			
			size_t tiles_per_row = atlas->tex_width / 4;
			size_t tile_idx = tile_y * tiles_per_row + tile_x;
			size_t pixel_in_tile = in_tile_y * 4 + in_tile_x;
			size_t tex_idx = (tile_idx * 16 + pixel_in_tile) * 2;  // 16 pixels per tile, 2 bytes per pixel
			
			u8 intensity = pixels[gy * pitch + gx];
			tex[tex_idx + 0] = intensity;  // Alpha
			tex[tex_idx + 1] = 0xFF;       // Intensity (white, color comes from vertex)
		}
	}
	
	DCFlushRange(atlas->texture_data, atlas->tex_width * atlas->tex_height * 2);
	MarkTexturesDirty();
}

void RenderBackend_PrepareToDrawGlyphs(RenderBackend_GlyphAtlas *atlas, RenderBackend_Surface *destination_surface, unsigned char red, unsigned char green, unsigned char blue)
{
	glyph_atlas = atlas;
	glyph_dest = destination_surface;
	glyph_r = red;
	glyph_g = green;
	glyph_b = blue;
}

// Helper to get IA8 pixel from tiled glyph atlas
static inline void GetIA8Pixel(u8 *tex, size_t tex_width, size_t px, size_t py, u8 *intensity, u8 *alpha)
{
	size_t tile_x = px / 4;
	size_t tile_y = py / 4;
	size_t in_tile_x = px % 4;
	size_t in_tile_y = py % 4;
	size_t tiles_per_row = tex_width / 4;
	size_t tile_idx = tile_y * tiles_per_row + tile_x;
	size_t pixel_in_tile = in_tile_y * 4 + in_tile_x;
	size_t byte_idx = tile_idx * 32 + pixel_in_tile * 2;
	
	*alpha = tex[byte_idx];
	*intensity = tex[byte_idx + 1];
}

// Switch to 640x480 projection for high-res text rendering
static void SetupHiResTextProjection(void)
{
	if (!in_hires_text_mode)
	{
		Mtx44 ortho;
		guOrtho(ortho, 0, GAME_HEIGHT * 2, 0, GAME_WIDTH * 2, 0, 1);  // 640x480 projection
		GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
		in_hires_text_mode = true;
	}
}

// Restore normal 320x240 projection
static void RestoreNormalProjection(void)
{
	if (in_hires_text_mode)
	{
		Mtx44 ortho;
		guOrtho(ortho, 0, GAME_HEIGHT, 0, GAME_WIDTH, 0, 1);  // 320x240 projection
		GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
		in_hires_text_mode = false;
	}
}

void RenderBackend_DrawGlyph(long x, long y, size_t gx, size_t gy, size_t gw, size_t gh)
{
	if (!glyph_atlas || !glyph_atlas->texture_data) return;
	
	// Draw to non-framebuffer surface: copy pixels directly at full resolution
	// (Draw.cpp handles coordinate scaling for hi-res text surfaces)
	if (glyph_dest != framebuffer_surface)
	{
		if (!glyph_dest || !glyph_dest->texture_data) return;
		
		// Ensure GPU is done before we modify the destination texture
		GX_DrawDone();
		
		u8 *src_tex = (u8*)glyph_atlas->texture_data;
		u16 *dst_tex = (u16*)glyph_dest->texture_data;
		
		// Draw glyph at full resolution (10x20 font)
		for (size_t py = 0; py < gh; py++)
		{
			for (size_t px = 0; px < gw; px++)
			{
				long sx = gx + px;
				long sy = gy + py;
				long dx = x + px;
				long dy = y + py;
				
				if (dx < 0 || dy < 0 || (size_t)dx >= glyph_dest->width || (size_t)dy >= glyph_dest->height)
					continue;
				
				// Get alpha from glyph atlas (IA8 format)
				u8 intensity, alpha;
				GetIA8Pixel(src_tex, glyph_atlas->tex_width, sx, sy, &intensity, &alpha);
				
				// Skip transparent pixels
				if (alpha < 16) continue;
				
				// Create colored pixel with the glyph color
				u16 pixel = RGB_to_RGB5A3(glyph_r, glyph_g, glyph_b, false);
				SetTiledPixel(dst_tex, glyph_dest->tex_width, dx, dy, pixel);
			}
		}
		
		// Flush texture and mark for cache invalidation
		DCFlushRange(glyph_dest->texture_data, glyph_dest->tex_width * glyph_dest->tex_height * 2);
		MarkTexturesDirty();
		return;
	}
	
	// Draw to framebuffer using GX at 640x480 resolution for sharp text
	SyncAndInvalidateTextures();
	
	// Switch to 640x480 projection for high-res text
	SetupHiResTextProjection();
	
	// Setup TEV for text: color = vertex color, alpha = texture alpha
	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
	
	// Alpha blend for text
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_GREATER, 8, GX_AOP_AND, GX_ALWAYS, 0);
	
	GX_LoadTexObj(&glyph_atlas->texObj, GX_TEXMAP0);
	
	// Texture coords (from the 10x20 glyph atlas)
	float tex_w = (float)glyph_atlas->tex_width;
	float tex_h = (float)glyph_atlas->tex_height;
	float s0 = (float)gx / tex_w;
	float t0 = (float)gy / tex_h;
	float s1 = (float)(gx + gw) / tex_w;
	float t1 = (float)(gy + gh) / tex_h;
	
	// Scale coordinates to 640x480 (input x,y are in 320x240 space)
	// The glyph size (gw, gh) is already at 10x20 scale
	float x0 = (float)x * 2.0f;
	float y0 = (float)y * 2.0f;
	float x1 = x0 + (float)gw;  // gw is already 10 (not 5), so no extra scaling
	float y1 = y0 + (float)gh;  // gh is already 20 (not 10)
	
	// Draw glyph at full resolution
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(x0, y0);
		GX_Color4u8(glyph_r, glyph_g, glyph_b, 0xFF);
		GX_TexCoord2f32(s0, t0);
		
		GX_Position2f32(x1, y0);
		GX_Color4u8(glyph_r, glyph_g, glyph_b, 0xFF);
		GX_TexCoord2f32(s1, t0);
		
		GX_Position2f32(x1, y1);
		GX_Color4u8(glyph_r, glyph_g, glyph_b, 0xFF);
		GX_TexCoord2f32(s1, t1);
		
		GX_Position2f32(x0, y1);
		GX_Color4u8(glyph_r, glyph_g, glyph_b, 0xFF);
		GX_TexCoord2f32(s0, t1);
	GX_End();
	GX_DrawDone();  // Force complete sync - diagnostic for tile corruption
	
	// Restore state
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
}

void RenderBackend_HandleRenderTargetLoss(void) {}
void RenderBackend_HandleWindowResize(size_t width, size_t height) { (void)width; (void)height; }
