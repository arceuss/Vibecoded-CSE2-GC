// Released under the MIT licence.
// See LICENCE.txt for details.

// GameCube GX Hardware Renderer for CSE2
// Rewritten with state machine for GX setup and explicit cache management

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
#include <ogc/system.h>

#define GC_LOG(fmt, ...) printf("[GX Render] " fmt "\n", ##__VA_ARGS__)

// Memory debugging
static u32 initial_arena_size = 0;
static u32 min_arena_size = 0xFFFFFFFF;  // Track lowest seen

// Safe allocation with logging
static void* SafeMalloc(size_t size, const char* what)
{
	void* ptr = malloc(size);
	if (!ptr && size > 0)
	{
		u32 arena_free = SYS_GetArena1Size();
		printf("[FATAL] malloc(%zu) failed for %s! Arena free: %uKB\n", 
			size, what, arena_free / 1024);
		// Hang so we can see the message
		while(1) { VIDEO_WaitVSync(); }
	}
	return ptr;
}

static void* SafeMemalign(size_t align, size_t size, const char* what)
{
	void* ptr = memalign(align, size);
	if (!ptr && size > 0)
	{
		u32 arena_free = SYS_GetArena1Size();
		printf("[FATAL] memalign(%zu, %zu) failed for %s! Arena free: %uKB\n", 
			align, size, what, arena_free / 1024);
		// Hang so we can see the message
		while(1) { VIDEO_WaitVSync(); }
	}
	return ptr;
}

// Game resolution - 320x240, GX hardware scales to 640x480 output
#define GAME_WIDTH 320
#define GAME_HEIGHT 240
#define EFB_WIDTH 640
#define EFB_HEIGHT 480

// GX texture format - use RGB5A3 for color key support
// RGB5A3: if MSB=1: RGB555, if MSB=0: ARGB3444
#define TEXTURE_FORMAT GX_TF_RGB5A3

//==============================================================================
// GX State Machine
//==============================================================================

typedef enum {
	GX_DRAW_STATE_NONE = 0,     // Unknown/uninitialized
	GX_DRAW_STATE_TEXTURED,     // Textured quad (pos + texcoord)
	GX_DRAW_STATE_COLORFILL,    // Color fill (pos + color)
	GX_DRAW_STATE_GLYPH,        // Glyph rendering (pos + color + texcoord)
} GXDrawState;

typedef enum {
	GX_PROJ_NORMAL = 0,         // 320x240 projection
	GX_PROJ_HIRES,              // 640x480 projection
} GXProjectionState;

typedef enum {
	GX_BLEND_NONE = 0,          // No blending
	GX_BLEND_ALPHA,             // Alpha blending
} GXBlendState;

// Current GX state tracking
static GXDrawState current_draw_state = GX_DRAW_STATE_NONE;
static GXProjectionState current_projection = GX_PROJ_NORMAL;
static GXBlendState current_blend = GX_BLEND_NONE;
static bool alpha_test_enabled = false;
static u8 alpha_test_threshold = 0;

// Texture cache tracking
static bool textures_dirty = false;
static bool gpu_pending = false;  // True if there are pending GPU commands

//==============================================================================
// Surface Structure
//==============================================================================

struct RenderBackend_Surface {
	GXTexObj texObj;
	void *texture_data;     // 32-byte aligned texture data
	size_t width;
	size_t height;
	size_t tex_width;       // Power of 2 width
	size_t tex_height;      // Power of 2 height
	size_t tex_size;        // Total texture size in bytes
	bool has_colorkey;      // Has transparent pixels
	bool is_hires;          // Hi-res text surface
	bool texture_valid;     // Texture cache is valid (flushed and not modified)
};

struct RenderBackend_GlyphAtlas {
	GXTexObj texObj;
	void *texture_data;
	size_t width;
	size_t height;
	size_t tex_width;
	size_t tex_height;
	size_t tex_size;
	bool texture_valid;
};

//==============================================================================
// Global State
//==============================================================================

static GXRModeObj *vmode = NULL;
static void *xfb[2] = {NULL, NULL};
static int whichfb = 0;
static void *gp_fifo = NULL;

static RenderBackend_Surface *framebuffer_surface = NULL;
static RenderBackend_Surface *current_target = NULL;

// Glyph rendering state
static RenderBackend_GlyphAtlas *glyph_atlas = NULL;
static RenderBackend_Surface *glyph_dest = NULL;
static u8 glyph_r, glyph_g, glyph_b;

// Pre-allocated buffer for EFB copies (BackupSurface)
// This avoids repeated large allocations that can fragment memory
// Max size: 512x256 in RGBA8 = 512KB (covers most use cases)
#define EFB_COPY_MAX_WIDTH 512
#define EFB_COPY_MAX_HEIGHT 256
#define EFB_COPY_BUFFER_SIZE (EFB_COPY_MAX_WIDTH * EFB_COPY_MAX_HEIGHT * 4)
static void *efb_copy_buffer = NULL;

// Debug
static int frame_count = 0;

//==============================================================================
// Utility Functions
//==============================================================================

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

// Get pixel from tiled RGB5A3 texture (4x4 tiles)
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

// Set pixel in tiled RGB5A3 texture (4x4 tiles)
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

// Get pixel from tiled IA8 texture (4x4 tiles)
static inline void GetIA8Pixel(u8 *tex, size_t tex_width, size_t px, size_t py, u8 *alpha, u8 *intensity)
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

//==============================================================================
// GPU Synchronization and Cache Management
//==============================================================================

// Wait for GPU to finish all pending commands
static inline void GPU_Sync(void)
{
	if (gpu_pending)
	{
		GX_DrawDone();
		gpu_pending = false;
	}
}

// Mark that GPU has pending work
static inline void GPU_MarkPending(void)
{
	gpu_pending = true;
}

// Invalidate texture cache (call after textures modified)
static void InvalidateTextureCache(void)
{
	GPU_Sync();
	GX_InvalidateTexAll();
	textures_dirty = false;
}

// Mark textures as needing invalidation
static inline void MarkTexturesDirty(void)
{
	textures_dirty = true;
}

// Ensure texture cache is valid before drawing
// Note: Since we invalidate all caches at frame start (following libogc examples),
// mid-frame invalidation is only needed if textures were modified THIS frame
static inline void EnsureTextureCacheValid(void)
{
	if (textures_dirty)
	{
		// Texture was modified mid-frame, need to sync and invalidate
		GPU_Sync();
		GX_InvalidateTexAll();
		textures_dirty = false;
	}
}

// Flush surface texture to main memory and mark dirty
static void FlushSurfaceTexture(RenderBackend_Surface *surface)
{
	if (surface && surface->texture_data)
	{
		DCFlushRange(surface->texture_data, surface->tex_size);
		surface->texture_valid = true;
		MarkTexturesDirty();
	}
}

// Flush atlas texture to main memory and mark dirty
static void FlushAtlasTexture(RenderBackend_GlyphAtlas *atlas)
{
	if (atlas && atlas->texture_data)
	{
		DCFlushRange(atlas->texture_data, atlas->tex_size);
		atlas->texture_valid = true;
		MarkTexturesDirty();
	}
}

// Prepare surface for CPU modification
static void PrepareSurfaceForCPUWrite(RenderBackend_Surface *surface)
{
	if (!surface || !surface->texture_data || surface == framebuffer_surface)
		return;
	
	// Wait for GPU to finish reading the texture
	GPU_Sync();
	
	// Invalidate CPU cache to ensure we see latest data (if GPU wrote to it)
	DCInvalidateRange(surface->texture_data, surface->tex_size);
	
	surface->texture_valid = false;
}

// Prepare atlas for CPU modification
static void PrepareAtlasForCPUWrite(RenderBackend_GlyphAtlas *atlas)
{
	if (!atlas || !atlas->texture_data)
		return;
	
	GPU_Sync();
	DCInvalidateRange(atlas->texture_data, atlas->tex_size);
	atlas->texture_valid = false;
}

//==============================================================================
// GX State Machine Functions
//==============================================================================

// Set projection matrix
static void SetProjection(GXProjectionState proj)
{
	if (current_projection != proj)
	{
		Mtx44 ortho;
		if (proj == GX_PROJ_HIRES)
		{
			guOrtho(ortho, 0, EFB_HEIGHT, 0, EFB_WIDTH, 0, 1);
		}
		else
		{
			guOrtho(ortho, 0, GAME_HEIGHT, 0, GAME_WIDTH, 0, 1);
		}
		GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
		current_projection = proj;
	}
}

// Set blend mode
static void SetBlendMode(GXBlendState blend)
{
	if (current_blend != blend)
	{
		if (blend == GX_BLEND_ALPHA)
		{
			GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
		}
		else
		{
			GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
		}
		current_blend = blend;
	}
}

// Set alpha test
static void SetAlphaTest(bool enable, u8 threshold)
{
	if (alpha_test_enabled != enable || alpha_test_threshold != threshold)
	{
		if (enable)
		{
			GX_SetAlphaCompare(GX_GREATER, threshold, GX_AOP_AND, GX_ALWAYS, 0);
		}
		else
		{
			GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
		}
		alpha_test_enabled = enable;
		alpha_test_threshold = threshold;
	}
}

// Setup GX state for textured quad drawing
static void SetDrawState_Textured(void)
{
	if (current_draw_state != GX_DRAW_STATE_TEXTURED)
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
		
		current_draw_state = GX_DRAW_STATE_TEXTURED;
	}
}

// Setup GX state for solid color fill
static void SetDrawState_ColorFill(void)
{
	if (current_draw_state != GX_DRAW_STATE_COLORFILL)
	{
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
		
		current_draw_state = GX_DRAW_STATE_COLORFILL;
	}
}

// Setup GX state for glyph rendering (texture + vertex color)
static void SetDrawState_Glyph(void)
{
	if (current_draw_state != GX_DRAW_STATE_GLYPH)
	{
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
		
		current_draw_state = GX_DRAW_STATE_GLYPH;
	}
}

// Force full state setup at frame start (synchronizes tracking with actual GPU state)
static void ForceDefaultState(void)
{
	// Force textured draw state as default
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
	
	current_draw_state = GX_DRAW_STATE_TEXTURED;
	
	// Force blend mode to none
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	current_blend = GX_BLEND_NONE;
	
	// Force alpha test to default (enabled with threshold 0)
	GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
	alpha_test_enabled = true;
	alpha_test_threshold = 0;
	
	// Force normal projection
	Mtx44 ortho;
	guOrtho(ortho, 0, GAME_HEIGHT, 0, GAME_WIDTH, 0, 1);
	GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
	current_projection = GX_PROJ_NORMAL;
}

//==============================================================================
// Initial GX Setup
//==============================================================================

static void InitialGXSetup(void)
{
	Mtx identity;
	
	// Set viewport to full EFB
	GX_SetViewport(0, 0, vmode->fbWidth, vmode->efbHeight, 0, 1);
	
	// Set scissor to full EFB
	GX_SetScissor(0, 0, vmode->fbWidth, vmode->efbHeight);
	
	// Identity modelview
	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	
	// Z compare location before texture
	GX_SetZCompLoc(GX_TRUE);
	
	// No Z buffer for 2D
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	
	// Cull none
	GX_SetCullMode(GX_CULL_NONE);
	
	// Invalidate texture cache
	GX_InvalidateTexAll();
	
	// Force default state (sets projection, vertex format, TEV, blend, alpha test)
	ForceDefaultState();
}

//==============================================================================
// Public API - Initialization
//==============================================================================

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
	
	// Allocate XFB (double buffered)
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
	
	// Initialize GX with 256KB FIFO
	gp_fifo = memalign(32, 256 * 1024);
	if (!gp_fifo)
	{
		GC_LOG("ERROR: FIFO alloc failed!");
		return NULL;
	}
	memset(gp_fifo, 0, 256 * 1024);
	DCFlushRange(gp_fifo, 256 * 1024);
	
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
	
	// Pixel format based on AA mode
	if (vmode->aa)
		GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
	else
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	
	// Initial GX setup
	InitialGXSetup();
	
	// Create framebuffer surface (represents the screen)
	framebuffer_surface = (RenderBackend_Surface*)malloc(sizeof(RenderBackend_Surface));
	if (!framebuffer_surface)
	{
		GC_LOG("ERROR: fb surface alloc failed!");
		return NULL;
	}
	memset(framebuffer_surface, 0, sizeof(RenderBackend_Surface));
	framebuffer_surface->width = GAME_WIDTH;
	framebuffer_surface->height = GAME_HEIGHT;
	framebuffer_surface->texture_valid = true;  // Screen is always "valid"
	
	current_target = framebuffer_surface;
	
	// Allocate EFB copy buffer (used by BackupSurface)
	efb_copy_buffer = memalign(32, EFB_COPY_BUFFER_SIZE);
	if (!efb_copy_buffer)
	{
		GC_LOG("WARNING: EFB copy buffer alloc failed - BackupSurface will be disabled");
	}
	else
	{
		memset(efb_copy_buffer, 0, EFB_COPY_BUFFER_SIZE);
		GC_LOG("EFB copy buffer: %d KB", EFB_COPY_BUFFER_SIZE / 1024);
	}
	
	// Ensure GPU is ready
	GX_DrawDone();
	GX_InvalidateTexAll();
	
	// Record initial memory state
	initial_arena_size = SYS_GetArena1Size();
	min_arena_size = initial_arena_size;
	GC_LOG("Init complete! Free Arena: %uKB", initial_arena_size / 1024);
	
	return framebuffer_surface;
}

void RenderBackend_Deinit(void)
{
	GPU_Sync();
	
	if (efb_copy_buffer)
	{
		free(efb_copy_buffer);
		efb_copy_buffer = NULL;
	}
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

//==============================================================================
// Public API - Frame Management
//==============================================================================

void RenderBackend_DrawScreen(void)
{
	frame_count++;
	
	// Ensure normal projection at frame end
	SetProjection(GX_PROJ_NORMAL);
	
	// Ensure all pending draws complete and flush FIFO
	GX_DrawDone();
	gpu_pending = false;
	
	// Copy EFB to XFB
	GX_CopyDisp(xfb[whichfb], GX_TRUE);
	
	// Wait for copy to complete
	GX_DrawDone();
	GX_Flush();
	
	// Flip buffers
	VIDEO_SetNextFramebuffer(xfb[whichfb]);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	whichfb ^= 1;
	
	// Clear for next frame
	GXColor bg = {0, 0, 0, 0xFF};
	GX_SetCopyClear(bg, GX_MAX_Z24);
	
	// === FRAME START SETUP (for next frame) ===
	// Following libogc examples pattern: invalidate ALL caches at frame start
	// This must be done BEFORE any drawing of the next frame
	GX_InvVtxCache();
	GX_InvalidateTexAll();
	textures_dirty = false;
	
	// Reset draw state so first draw of next frame sets up vertex descriptors fresh
	current_draw_state = GX_DRAW_STATE_NONE;
	
	// Memory debugging - check every 5 seconds (300 frames at 60fps)
	if (frame_count % 300 == 0)
	{
		u32 arena_size = SYS_GetArena1Size();
		if (initial_arena_size == 0)
			initial_arena_size = arena_size;
		if (arena_size < min_arena_size)
			min_arena_size = arena_size;
		
		u32 used = initial_arena_size - arena_size;
		GC_LOG("Frame %d - Arena: %uKB free (used: %uKB, min: %uKB)", 
			frame_count, arena_size / 1024, used / 1024, min_arena_size / 1024);
	}
}

//==============================================================================
// Public API - Surface Management
//==============================================================================

RenderBackend_Surface* RenderBackend_CreateSurface(size_t width, size_t height, bool render_target)
{
	(void)render_target;
	
	RenderBackend_Surface *surface = (RenderBackend_Surface*)SafeMalloc(sizeof(RenderBackend_Surface), "surface struct");
	if (!surface) return NULL;
	
	memset(surface, 0, sizeof(RenderBackend_Surface));
	surface->width = width;
	surface->height = height;
	surface->tex_width = NextPow2(width);
	surface->tex_height = NextPow2(height);
	surface->tex_size = surface->tex_width * surface->tex_height * 2;  // RGB5A3 = 2 bytes
	
	// Allocate 32-byte aligned texture data
	surface->texture_data = SafeMemalign(32, surface->tex_size, "surface texture");
	if (!surface->texture_data)
	{
		GC_LOG("CreateSurface FAILED: %zux%zu", width, height);
		free(surface);
		return NULL;
	}
	
	// Clear texture data
	memset(surface->texture_data, 0, surface->tex_size);
	
	// Flush to main memory
	DCFlushRange(surface->texture_data, surface->tex_size);
	
	// Initialize texture object
	GX_InitTexObj(&surface->texObj, surface->texture_data,
		surface->tex_width, surface->tex_height,
		TEXTURE_FORMAT, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&surface->texObj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	
	surface->texture_valid = true;
	
	GC_LOG("CreateSurface: %zux%zu (tex: %zux%zu)", width, height, surface->tex_width, surface->tex_height);
	return surface;
}

void RenderBackend_FreeSurface(RenderBackend_Surface *surface)
{
	if (surface && surface != framebuffer_surface)
	{
		// Ensure GPU isn't using this texture
		GPU_Sync();
		
		if (surface->texture_data)
			free(surface->texture_data);
		free(surface);
	}
}

bool RenderBackend_IsSurfaceLost(RenderBackend_Surface *surface) { (void)surface; return false; }
void RenderBackend_RestoreSurface(RenderBackend_Surface *surface) { (void)surface; }

void RenderBackend_SetSurfaceHiRes(RenderBackend_Surface *surface, bool hires)
{
	if (surface && surface != framebuffer_surface)
	{
		surface->is_hires = hires;
		if (hires)
			GC_LOG("Surface marked as hi-res: %zux%zu", surface->width, surface->height);
	}
}

//==============================================================================
// Public API - Surface Upload
//==============================================================================

void RenderBackend_UploadSurface(RenderBackend_Surface *surface, const unsigned char *pixels, size_t width, size_t height)
{
	if (!surface || !pixels || surface == framebuffer_surface) return;
	
	// Prepare for CPU write
	PrepareSurfaceForCPUWrite(surface);
	
	// Reallocate if size changed
	if (surface->width != width || surface->height != height)
	{
		surface->width = width;
		surface->height = height;
		surface->tex_width = NextPow2(width);
		surface->tex_height = NextPow2(height);
		surface->tex_size = surface->tex_width * surface->tex_height * 2;
		
		if (surface->texture_data)
			free(surface->texture_data);
		
		surface->texture_data = SafeMemalign(32, surface->tex_size, "surface resize");
		if (!surface->texture_data) return;
		memset(surface->texture_data, 0, surface->tex_size);
		
		GX_InitTexObj(&surface->texObj, surface->texture_data,
			surface->tex_width, surface->tex_height,
			TEXTURE_FORMAT, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjLOD(&surface->texObj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	}
	
	// Convert RGB to RGB5A3 with tiled layout (4x4 tiles)
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
	
	// Flush texture and mark dirty
	FlushSurfaceTexture(surface);
}

//==============================================================================
// Public API - Blit Operations
//==============================================================================

void RenderBackend_Blit(RenderBackend_Surface *source_surface, const RenderBackend_Rect *rect, RenderBackend_Surface *destination_surface, long x, long y, bool colour_key)
{
	if (!source_surface || !rect) return;
	
	//--------------------------------------------------------------------------
	// Special case: BackupSurface (EFB to texture)
	//--------------------------------------------------------------------------
	if (source_surface == framebuffer_surface)
	{
		if (!destination_surface || !destination_surface->texture_data) return;
		
		// Check if EFB copy buffer is available
		if (!efb_copy_buffer)
		{
			GC_LOG("BackupSurface: No EFB copy buffer available");
			return;
		}
		
		// Wait for all GPU commands to complete before EFB access
		GPU_Sync();
		
		// Calculate EFB coordinates (game is 320x240, EFB is 640x480)
		u16 efb_x = (u16)(rect->left * 2);
		u16 efb_y = (u16)(rect->top * 2);
		u16 efb_w = (u16)((rect->right - rect->left) * 2);
		u16 efb_h = (u16)((rect->bottom - rect->top) * 2);
		
		// EFB copy requires even coordinates
		efb_x &= ~1;
		efb_y &= ~1;
		efb_w = (efb_w + 1) & ~1;
		efb_h = (efb_h + 1) & ~1;
		
		// Clamp to EFB bounds
		if (efb_x + efb_w > EFB_WIDTH) efb_w = EFB_WIDTH - efb_x;
		if (efb_y + efb_h > EFB_HEIGHT) efb_h = EFB_HEIGHT - efb_y;
		
		// Temp texture dimensions (power of 2)
		size_t temp_tex_w = NextPow2(efb_w);
		size_t temp_tex_h = NextPow2(efb_h);
		
		// Clamp to pre-allocated buffer size
		if (temp_tex_w > EFB_COPY_MAX_WIDTH) temp_tex_w = EFB_COPY_MAX_WIDTH;
		if (temp_tex_h > EFB_COPY_MAX_HEIGHT) temp_tex_h = EFB_COPY_MAX_HEIGHT;
		
		// Recalculate EFB dimensions to match buffer constraints
		if (efb_w > temp_tex_w) efb_w = temp_tex_w;
		if (efb_h > temp_tex_h) efb_h = temp_tex_h;
		
		size_t temp_size = temp_tex_w * temp_tex_h * 4;  // RGBA8
		
		// Use pre-allocated buffer instead of allocating every time
		void *temp_tex = efb_copy_buffer;
		memset(temp_tex, 0, temp_size);
		
		// Invalidate cache before GPU writes
		DCInvalidateRange(temp_tex, temp_size);
		
		// Setup EFB copy
		GX_SetTexCopySrc(efb_x, efb_y, efb_w, efb_h);
		GX_SetTexCopyDst(temp_tex_w, temp_tex_h, GX_TF_RGBA8, GX_FALSE);
		
		// Perform copy
		GX_CopyTex(temp_tex, GX_FALSE);
		
		// Synchronize
		GX_PixModeSync();
		GX_DrawDone();
		
		// Invalidate cache so CPU sees GPU's writes
		DCInvalidateRange(temp_tex, temp_size);
		
		// Prepare destination for CPU write
		PrepareSurfaceForCPUWrite(destination_surface);
		
		// Downsample from RGBA8 to RGB5A3
		u8 *src_tex = (u8*)temp_tex;
		u16 *dst_tex = (u16*)destination_surface->texture_data;
		
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
				
				// Sample from EFB at 2x coordinates
				size_t sx = px * 2;
				size_t sy = py * 2;
				
				// Read RGBA8 from tiled format (4x4 tiles, AR then GB)
				size_t tile_x = sx / 4;
				size_t tile_y = sy / 4;
				size_t in_tile_x = sx % 4;
				size_t in_tile_y = sy % 4;
				size_t tiles_per_row = temp_tex_w / 4;
				size_t tile_idx = tile_y * tiles_per_row + tile_x;
				size_t pixel_in_tile = in_tile_y * 4 + in_tile_x;
				
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
					pixel = ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
				}
				else
				{
					pixel = 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
				}
				
				SetTiledPixel(dst_tex, destination_surface->tex_width, dx, dy, pixel);
			}
		}
		
		// Note: temp_tex is pre-allocated buffer, don't free it
		
		// Flush destination and mark dirty
		FlushSurfaceTexture(destination_surface);
		
		GC_LOG("BackupSurface: EFB (%d,%d %dx%d) -> surface", efb_x, efb_y, efb_w, efb_h);
		return;
	}
	
	// Source must have texture data for remaining cases
	if (!source_surface->texture_data) return;
	
	//--------------------------------------------------------------------------
	// Draw to framebuffer using GX
	//--------------------------------------------------------------------------
	if (destination_surface == framebuffer_surface)
	{
		// Set projection based on source type
		if (source_surface->is_hires)
		{
			SetProjection(GX_PROJ_HIRES);
		}
		else
		{
			SetProjection(GX_PROJ_NORMAL);
		}
		
		// Ensure texture cache is valid
		EnsureTextureCacheValid();
		
		// Setup draw state
		SetDrawState_Textured();
		SetBlendMode(GX_BLEND_NONE);
		SetAlphaTest(colour_key && source_surface->has_colorkey, 0);
		
		// Calculate texture coordinates
		float tex_w = (float)source_surface->tex_width;
		float tex_h = (float)source_surface->tex_height;
		
		float s0 = (float)rect->left / tex_w;
		float t0 = (float)rect->top / tex_h;
		float s1 = (float)rect->right / tex_w;
		float t1 = (float)rect->bottom / tex_h;
		
		// Calculate screen coordinates
		float scale = source_surface->is_hires ? 2.0f : 1.0f;
		float x0 = (float)x * scale;
		float y0 = (float)y * scale;
		float x1 = x0 + (rect->right - rect->left);
		float y1 = y0 + (rect->bottom - rect->top);
		
		// Load texture
		GX_LoadTexObj(&source_surface->texObj, GX_TEXMAP0);
		
		// Draw quad
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
		
		GPU_MarkPending();
		return;
	}
	
	//--------------------------------------------------------------------------
	// Surface-to-surface copy (CPU)
	//--------------------------------------------------------------------------
	if (!destination_surface || !destination_surface->texture_data) return;
	
	// Prepare destination for CPU write
	PrepareSurfaceForCPUWrite(destination_surface);
	
	u16 *src_tex = (u16*)source_surface->texture_data;
	u16 *dst_tex = (u16*)destination_surface->texture_data;
	
	long src_x = rect->left;
	long src_y = rect->top;
	long width = rect->right - rect->left;
	long height = rect->bottom - rect->top;
	
	// Check if scaling up (normal-res to hi-res)
	bool scale_up = destination_surface->is_hires && !source_surface->is_hires;
	
	for (long py = 0; py < height; py++)
	{
		for (long px = 0; px < width; px++)
		{
			long sx = src_x + px;
			long sy = src_y + py;
			
			if (sx < 0 || sy < 0 || (size_t)sx >= source_surface->tex_width || (size_t)sy >= source_surface->tex_height)
				continue;
			
			u16 pixel = GetTiledPixel(src_tex, source_surface->tex_width, sx, sy);
			
			// Skip transparent pixels if color key enabled
			if (colour_key && (pixel & 0x8000) == 0 && (pixel & 0x7000) == 0)
				continue;
			
			if (scale_up)
			{
				// Scale up 2x
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
				long dx = x + px;
				long dy = y + py;
				
				if (dx < 0 || dy < 0 || (size_t)dx >= destination_surface->width || (size_t)dy >= destination_surface->height)
					continue;
				
				SetTiledPixel(dst_tex, destination_surface->tex_width, dx, dy, pixel);
			}
		}
	}
	
	// Transfer colorkey flag
	if (source_surface->has_colorkey)
		destination_surface->has_colorkey = true;
	
	// Flush texture and mark dirty
	FlushSurfaceTexture(destination_surface);
}

//==============================================================================
// Public API - Color Fill
//==============================================================================

void RenderBackend_ColourFill(RenderBackend_Surface *surface, const RenderBackend_Rect *rect, unsigned char red, unsigned char green, unsigned char blue)
{
	if (!rect) return;
	
	//--------------------------------------------------------------------------
	// Fill framebuffer using GX
	//--------------------------------------------------------------------------
	if (surface == framebuffer_surface)
	{
		SetProjection(GX_PROJ_NORMAL);
		EnsureTextureCacheValid();
		
		SetDrawState_ColorFill();
		SetBlendMode(GX_BLEND_NONE);
		SetAlphaTest(false, 0);
		
		float x0 = (float)rect->left;
		float y0 = (float)rect->top;
		float x1 = (float)rect->right;
		float y1 = (float)rect->bottom;
		
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
		
		GPU_MarkPending();
		return;
	}
	
	//--------------------------------------------------------------------------
	// Fill surface texture (CPU)
	//--------------------------------------------------------------------------
	if (!surface || !surface->texture_data) return;
	
	// Prepare for CPU write
	PrepareSurfaceForCPUWrite(surface);
	
	// Convert color to RGB5A3
	bool transparent = (red == 0 && green == 0 && blue == 0);
	u16 pixel = RGB_to_RGB5A3(red, green, blue, transparent);
	if (transparent) surface->has_colorkey = true;
	
	// Fill rect
	size_t x0 = rect->left;
	size_t y0 = rect->top;
	size_t x1 = rect->right;
	size_t y1 = rect->bottom;
	
	if (x1 > surface->width) x1 = surface->width;
	if (y1 > surface->height) y1 = surface->height;
	
	u16 *tex = (u16*)surface->texture_data;
	
	for (size_t py = y0; py < y1; py++)
	{
		for (size_t px = x0; px < x1; px++)
		{
			SetTiledPixel(tex, surface->tex_width, px, py, pixel);
		}
	}
	
	// Flush and mark dirty
	FlushSurfaceTexture(surface);
}

//==============================================================================
// Public API - Glyph Atlas Management
//==============================================================================

RenderBackend_GlyphAtlas* RenderBackend_CreateGlyphAtlas(size_t width, size_t height)
{
	RenderBackend_GlyphAtlas *atlas = (RenderBackend_GlyphAtlas*)SafeMalloc(sizeof(RenderBackend_GlyphAtlas), "glyph atlas struct");
	if (!atlas) return NULL;
	
	memset(atlas, 0, sizeof(RenderBackend_GlyphAtlas));
	atlas->width = width;
	atlas->height = height;
	atlas->tex_width = NextPow2(width);
	atlas->tex_height = NextPow2(height);
	atlas->tex_size = atlas->tex_width * atlas->tex_height * 2;  // IA8 = 2 bytes
	
	atlas->texture_data = SafeMemalign(32, atlas->tex_size, "glyph atlas texture");
	if (!atlas->texture_data)
	{
		free(atlas);
		return NULL;
	}
	memset(atlas->texture_data, 0, atlas->tex_size);
	
	// Flush to main memory
	DCFlushRange(atlas->texture_data, atlas->tex_size);
	
	GX_InitTexObj(&atlas->texObj, atlas->texture_data,
		atlas->tex_width, atlas->tex_height,
		GX_TF_IA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&atlas->texObj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	
	atlas->texture_valid = true;
	
	GC_LOG("CreateGlyphAtlas: %zux%zu (IA8)", width, height);
	return atlas;
}

void RenderBackend_DestroyGlyphAtlas(RenderBackend_GlyphAtlas *atlas)
{
	if (atlas)
	{
		GPU_Sync();
		
		if (atlas->texture_data)
			free(atlas->texture_data);
		free(atlas);
	}
}

void RenderBackend_UploadGlyph(RenderBackend_GlyphAtlas *atlas, size_t x, size_t y, const unsigned char *pixels, size_t width, size_t height, size_t pitch)
{
	if (!atlas || !atlas->texture_data || !pixels) return;
	
	// Prepare for CPU write
	PrepareAtlasForCPUWrite(atlas);
	
	// IA8 uses 4x4 tiles, 2 bytes per pixel
	u8 *tex = (u8*)atlas->texture_data;
	
	for (size_t gy = 0; gy < height; gy++)
	{
		for (size_t gx = 0; gx < width; gx++)
		{
			size_t dx = x + gx;
			size_t dy = y + gy;
			
			if (dx >= atlas->tex_width || dy >= atlas->tex_height) continue;
			
			// Calculate tiled offset
			size_t tile_x = dx / 4;
			size_t tile_y = dy / 4;
			size_t in_tile_x = dx % 4;
			size_t in_tile_y = dy % 4;
			
			size_t tiles_per_row = atlas->tex_width / 4;
			size_t tile_idx = tile_y * tiles_per_row + tile_x;
			size_t pixel_in_tile = in_tile_y * 4 + in_tile_x;
			size_t tex_idx = (tile_idx * 16 + pixel_in_tile) * 2;
			
			u8 intensity = pixels[gy * pitch + gx];
			tex[tex_idx + 0] = intensity;  // Alpha
			tex[tex_idx + 1] = 0xFF;       // Intensity (white)
		}
	}
	
	// Flush and mark dirty
	FlushAtlasTexture(atlas);
}

//==============================================================================
// Public API - Glyph Drawing
//==============================================================================

void RenderBackend_PrepareToDrawGlyphs(RenderBackend_GlyphAtlas *atlas, RenderBackend_Surface *destination_surface, unsigned char red, unsigned char green, unsigned char blue)
{
	glyph_atlas = atlas;
	glyph_dest = destination_surface;
	glyph_r = red;
	glyph_g = green;
	glyph_b = blue;
}

void RenderBackend_DrawGlyph(long x, long y, size_t gx, size_t gy, size_t gw, size_t gh)
{
	if (!glyph_atlas || !glyph_atlas->texture_data) return;
	
	//--------------------------------------------------------------------------
	// Draw to surface (CPU)
	//--------------------------------------------------------------------------
	if (glyph_dest != framebuffer_surface)
	{
		if (!glyph_dest || !glyph_dest->texture_data) return;
		
		// Prepare for CPU write
		PrepareSurfaceForCPUWrite(glyph_dest);
		
		u8 *src_tex = (u8*)glyph_atlas->texture_data;
		u16 *dst_tex = (u16*)glyph_dest->texture_data;
		
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
				
				u8 intensity, alpha;
				GetIA8Pixel(src_tex, glyph_atlas->tex_width, sx, sy, &alpha, &intensity);
				
				if (alpha < 16) continue;
				
				u16 pixel = RGB_to_RGB5A3(glyph_r, glyph_g, glyph_b, false);
				SetTiledPixel(dst_tex, glyph_dest->tex_width, dx, dy, pixel);
			}
		}
		
		// Flush and mark dirty
		FlushSurfaceTexture(glyph_dest);
		return;
	}
	
	//--------------------------------------------------------------------------
	// Draw to framebuffer using GX
	//--------------------------------------------------------------------------
	EnsureTextureCacheValid();
	
	// Use hi-res projection for sharp text
	SetProjection(GX_PROJ_HIRES);
	SetDrawState_Glyph();
	SetBlendMode(GX_BLEND_ALPHA);
	SetAlphaTest(true, 8);
	
	// Load atlas texture
	GX_LoadTexObj(&glyph_atlas->texObj, GX_TEXMAP0);
	
	// Calculate texture coordinates
	float tex_w = (float)glyph_atlas->tex_width;
	float tex_h = (float)glyph_atlas->tex_height;
	float s0 = (float)gx / tex_w;
	float t0 = (float)gy / tex_h;
	float s1 = (float)(gx + gw) / tex_w;
	float t1 = (float)(gy + gh) / tex_h;
	
	// Scale to 640x480
	float x0 = (float)x * 2.0f;
	float y0 = (float)y * 2.0f;
	float x1 = x0 + (float)gw;
	float y1 = y0 + (float)gh;
	
	// Draw glyph
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
	
	GPU_MarkPending();
}

//==============================================================================
// Public API - Misc
//==============================================================================

void RenderBackend_HandleRenderTargetLoss(void) {}
void RenderBackend_HandleWindowResize(size_t width, size_t height) { (void)width; (void)height; }
