// Released under the MIT licence.
// See LICENCE.txt for details.

// GameCube Software Window Backend for CSE2
// Handles framebuffer display via GX/VI

#include "../Software.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <gccore.h>
#include <malloc.h>

// Screen dimensions
static size_t screen_width;
static size_t screen_height;

// Software framebuffer (RGB24)
static unsigned char *software_framebuffer;
static size_t software_framebuffer_pitch;

// Hardware framebuffers (YUV for TV output)
static void *xfb[2] = {NULL, NULL};
static int current_fb = 0;

// Video mode
static GXRModeObj *rmode = NULL;

// Convert RGB to Y1CbY2Cr format for GameCube framebuffer
// GameCube uses YUV 4:2:2 format where each 32-bit word contains two pixels
static inline void RGBToYCbCr(unsigned char r, unsigned char g, unsigned char b,
                               unsigned char *y, unsigned char *cb, unsigned char *cr)
{
	// Standard RGB to YCbCr conversion
	int y_val = 16 + ((66 * r + 129 * g + 25 * b + 128) >> 8);
	int cb_val = 128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8);
	int cr_val = 128 + ((112 * r - 94 * g - 18 * b + 128) >> 8);

	// Clamp values
	*y = (unsigned char)(y_val < 16 ? 16 : (y_val > 235 ? 235 : y_val));
	*cb = (unsigned char)(cb_val < 16 ? 16 : (cb_val > 240 ? 240 : cb_val));
	*cr = (unsigned char)(cr_val < 16 ? 16 : (cr_val > 240 ? 240 : cr_val));
}

bool WindowBackend_Software_CreateWindow(const char *window_title, size_t width, size_t height, bool fullscreen)
{
	(void)window_title;
	(void)fullscreen;

	screen_width = width;
	screen_height = height;

	// Get preferred video mode
	rmode = VIDEO_GetPreferredMode(NULL);

	// Allocate framebuffers in uncached memory
	xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

	if (xfb[0] == NULL || xfb[1] == NULL)
	{
		return false;
	}

	// Clear framebuffers
	VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
	VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);

	// Configure the video system
	VIDEO_Configure(rmode);

	// Set the first framebuffer
	VIDEO_SetNextFramebuffer(xfb[current_fb]);

	// Make the display visible
	VIDEO_SetBlack(FALSE);

	// Flush register changes
	VIDEO_Flush();

	// Wait for video setup to complete
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	// Allocate software framebuffer (RGB24)
	software_framebuffer_pitch = screen_width * 3;
	software_framebuffer = (unsigned char *)malloc(screen_width * screen_height * 3);

	if (software_framebuffer == NULL)
	{
		return false;
	}

	// Clear software framebuffer
	memset(software_framebuffer, 0, screen_width * screen_height * 3);

	return true;
}

void WindowBackend_Software_DestroyWindow(void)
{
	if (software_framebuffer != NULL)
	{
		free(software_framebuffer);
		software_framebuffer = NULL;
	}

	// Note: xfb memory is managed by the system, don't free it manually
}

unsigned char *WindowBackend_Software_GetFramebuffer(size_t *pitch)
{
	*pitch = software_framebuffer_pitch;
	return software_framebuffer;
}

void WindowBackend_Software_Display(void)
{
	if (software_framebuffer == NULL || xfb[current_fb] == NULL)
		return;

	// Get destination framebuffer
	unsigned char *dest = (unsigned char *)xfb[current_fb];

	// Calculate scaling and centering
	// Game resolution (typically 320x240) needs to be scaled to TV resolution
	size_t dest_width = rmode->fbWidth;
	size_t dest_height = rmode->xfbHeight;

	// Calculate scale factors (integer scaling for crisp pixels)
	size_t scale_x = dest_width / screen_width;
	size_t scale_y = dest_height / screen_height;
	size_t scale = (scale_x < scale_y) ? scale_x : scale_y;
	if (scale < 1) scale = 1;

	// Calculate centered position
	size_t offset_x = (dest_width - (screen_width * scale)) / 2;
	size_t offset_y = (dest_height - (screen_height * scale)) / 2;

	// Clear the framebuffer first
	VIDEO_ClearFrameBuffer(rmode, xfb[current_fb], COLOR_BLACK);

	// Convert RGB24 to YCbYCr and write to framebuffer
	// GameCube framebuffer format: Y1 Cb Y2 Cr (two pixels per 32-bit word)
	for (size_t src_y = 0; src_y < screen_height; src_y++)
	{
		for (size_t sy = 0; sy < scale; sy++)
		{
			size_t dest_y = offset_y + (src_y * scale) + sy;
			if (dest_y >= dest_height)
				continue;

			// Process two source pixels at a time for YCbYCr format
			for (size_t src_x = 0; src_x < screen_width; src_x += 2)
			{
				// Get first pixel
				const unsigned char *pixel1 = &software_framebuffer[(src_y * software_framebuffer_pitch) + (src_x * 3)];
				unsigned char r1 = pixel1[0];
				unsigned char g1 = pixel1[1];
				unsigned char b1 = pixel1[2];

				// Get second pixel (or repeat first if at edge)
				const unsigned char *pixel2;
				if (src_x + 1 < screen_width)
				{
					pixel2 = &software_framebuffer[(src_y * software_framebuffer_pitch) + ((src_x + 1) * 3)];
				}
				else
				{
					pixel2 = pixel1;
				}
				unsigned char r2 = pixel2[0];
				unsigned char g2 = pixel2[1];
				unsigned char b2 = pixel2[2];

				// Convert to YCbCr
				unsigned char y1, cb1, cr1;
				unsigned char y2, cb2, cr2;
				RGBToYCbCr(r1, g1, b1, &y1, &cb1, &cr1);
				RGBToYCbCr(r2, g2, b2, &y2, &cb2, &cr2);

				// Average the chroma for better quality
				unsigned char cb = (cb1 + cb2) / 2;
				unsigned char cr = (cr1 + cr2) / 2;

				// Write scaled pixels
				for (size_t sx = 0; sx < scale; sx++)
				{
					size_t dest_x = offset_x + (src_x * scale) + (sx * 2);
					if (dest_x + 1 >= dest_width)
						continue;

					// Calculate destination offset
					// GameCube XFB is stored in a specific format
					size_t dest_offset = (dest_y * dest_width + dest_x) * 2;

					// Write Y1 Cb Y2 Cr
					dest[dest_offset + 0] = y1;
					dest[dest_offset + 1] = cb;
					dest[dest_offset + 2] = y2;
					dest[dest_offset + 3] = cr;
				}
			}
		}
	}

	// Swap framebuffers
	VIDEO_SetNextFramebuffer(xfb[current_fb]);
	VIDEO_Flush();
	VIDEO_WaitVSync();

	current_fb ^= 1;
}

void WindowBackend_Software_HandleWindowResize(size_t width, size_t height)
{
	(void)width;
	(void)height;
	// Window doesn't resize on GameCube
}

