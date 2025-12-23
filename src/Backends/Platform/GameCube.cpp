// Released under the MIT licence.
// See LICENCE.txt for details.

// GameCube Platform Backend for CSE2

#include "../Misc.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#include <gccore.h>
#include <fat.h>
#include <sdcard/gcsd.h>
#include <ogc/lwp_watchdog.h>

#define PLAT_LOG(fmt, ...) printf("[Platform] " fmt "\n", ##__VA_ARGS__)

// SD card mount status
static bool sd_mounted = false;
static const char* sd_mount_point = NULL;

static unsigned long ticks_per_second;
static u64 start_time;

// Callback storage (unused on GameCube but needed for interface)
static void (*drag_drop_callback)(const char *path);
static void (*focus_callback)(bool focus);

// Helper function to create directories recursively
static bool CreateDirectoryRecursive(const char *path)
{
	char tmp[256];
	char *p = NULL;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	
	// Remove trailing slash
	if (tmp[len - 1] == '/')
		tmp[len - 1] = 0;

	for (p = tmp + 1; *p; p++)
	{
		if (*p == '/')
		{
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	
	int result = mkdir(tmp, 0755);
	return (result == 0 || errno == EEXIST);
}

// Try to mount SD card from different sources
static bool MountSDCard(void)
{
	PLAT_LOG("Attempting to mount SD card...");
	
	// Try SD Gecko Slot B first (most common setup)
	PLAT_LOG("Trying SD Gecko Slot B...");
	if (fatMountSimple("sd", &__io_gcsdb))
	{
		sd_mounted = true;
		sd_mount_point = "sd:";
		PLAT_LOG("SD Gecko Slot B mounted successfully!");
		return true;
	}
	PLAT_LOG("SD Gecko Slot B failed");
	
	// Try SD Gecko Slot A as fallback
	PLAT_LOG("Trying SD Gecko Slot A...");
	if (fatMountSimple("sd", &__io_gcsda))
	{
		sd_mounted = true;
		sd_mount_point = "sd:";
		PLAT_LOG("SD Gecko Slot A mounted successfully!");
		return true;
	}
	PLAT_LOG("SD Gecko Slot A failed");
	
	// Try SD2SP2 (Serial Port 2) as last resort
	PLAT_LOG("Trying SD2SP2 (Serial Port 2)...");
	if (fatMountSimple("sd", &__io_gcsd2))
	{
		sd_mounted = true;
		sd_mount_point = "sd:";
		PLAT_LOG("SD2SP2 mounted successfully!");
		return true;
	}
	PLAT_LOG("SD2SP2 failed");
	
	// Try generic fatInitDefault as absolute fallback
	PLAT_LOG("Trying fatInitDefault...");
	if (fatInitDefault())
	{
		sd_mounted = true;
		sd_mount_point = "sd:";
		PLAT_LOG("fatInitDefault succeeded!");
		return true;
	}
	
	PLAT_LOG("All SD mount attempts failed!");
	return false;
}

bool Backend_Init(void (*drag_and_drop_callback)(const char *path), void (*window_focus_callback)(bool focus))
{
	drag_drop_callback = drag_and_drop_callback;
	focus_callback = window_focus_callback;

	// Enable logging to Dolphin FIRST
	SYS_STDIO_Report(true);

	PLAT_LOG("=== CSE2 GameCube Port ===");
	PLAT_LOG("Backend_Init starting...");

	// Initialize video subsystem
	VIDEO_Init();
	PLAT_LOG("VIDEO_Init done");

	// Initialize controller
	PAD_Init();
	PLAT_LOG("PAD_Init done");

	// Initialize FAT filesystem for SD card access
	// Try different SD card interfaces (SD Gecko Slot B, Slot A, SD2SP2)
	PLAT_LOG("Initializing SD card...");
	bool fat_ok = MountSDCard();
	PLAT_LOG("SD card init: %s", fat_ok ? "OK" : "FAILED (will use embedded data, saves disabled)");

	// If SD mounted, ensure save directory exists
	if (sd_mounted)
	{
#ifdef JAPANESE
		const char *save_dir = "sd:/cse2-jp";
#else
		const char *save_dir = "sd:/cse2";
#endif
		PLAT_LOG("Creating save directory: %s", save_dir);
		if (CreateDirectoryRecursive(save_dir))
		{
			PLAT_LOG("Save directory ready");
		}
		else
		{
			PLAT_LOG("Warning: Could not create save directory");
		}
	}

	// Get ticks per second for timing
	ticks_per_second = TB_TIMER_CLOCK; // Bus clock / 4
	start_time = gettime();

	PLAT_LOG("Backend_Init complete");
	return true;
}

void Backend_Deinit(void)
{
	// Nothing special needed for cleanup
}

void Backend_PostWindowCreation(void)
{
	// Nothing to do
}

bool Backend_GetPaths(std::string *module_path, std::string *data_path)
{
	// Use SD card path if mounted, otherwise indicate embedded data will be used
#ifdef JAPANESE
	*module_path = "sd:/cse2-jp";
#else
	*module_path = "sd:/cse2";
#endif

	*data_path = *module_path + "/data";

	PLAT_LOG("Backend_GetPaths: module=%s, data=%s", module_path->c_str(), data_path->c_str());
	PLAT_LOG("SD card mounted: %s", sd_mounted ? "YES (saves enabled)" : "NO (saves disabled)");
	return true;
}

// Check if SD card is available for saving
bool Backend_IsSDMounted(void)
{
	return sd_mounted;
}

void Backend_HideMouse(void)
{
	// No mouse on GameCube
}

void Backend_SetWindowIcon(const unsigned char *rgb_pixels, size_t width, size_t height)
{
	(void)rgb_pixels;
	(void)width;
	(void)height;
	// Not applicable on GameCube
}

void Backend_SetCursor(const unsigned char *rgba_pixels, size_t width, size_t height)
{
	(void)rgba_pixels;
	(void)width;
	(void)height;
	// Not applicable on GameCube
}

void Backend_EnableDragAndDrop(void)
{
	// Not applicable on GameCube
}

static int system_task_count = 0;

bool Backend_SystemTask(bool active)
{
	(void)active;

	system_task_count++;
	if (system_task_count <= 5 || system_task_count % 60 == 0)
	{
		PLAT_LOG("Backend_SystemTask called %d times", system_task_count);
	}

	// Check if we should exit (reset button, etc.)
	// For now, always return true (keep running)
	return true;
}

void Backend_GetKeyboardState(bool *keyboard_state)
{
	memset(keyboard_state, 0, sizeof(bool) * BACKEND_KEYBOARD_TOTAL);

	// Read GameCube controller
	PAD_ScanPads();

	u32 held = PAD_ButtonsHeld(0);
	s8 stick_x = PAD_StickX(0);
	s8 stick_y = PAD_StickY(0);

	// D-Pad and analog stick for movement
	const int STICK_THRESHOLD = 40;

	// Up
	keyboard_state[BACKEND_KEYBOARD_UP] = (held & PAD_BUTTON_UP) || (stick_y > STICK_THRESHOLD);
	// Down
	keyboard_state[BACKEND_KEYBOARD_DOWN] = (held & PAD_BUTTON_DOWN) || (stick_y < -STICK_THRESHOLD);
	// Left
	keyboard_state[BACKEND_KEYBOARD_LEFT] = (held & PAD_BUTTON_LEFT) || (stick_x < -STICK_THRESHOLD);
	// Right
	keyboard_state[BACKEND_KEYBOARD_RIGHT] = (held & PAD_BUTTON_RIGHT) || (stick_x > STICK_THRESHOLD);

	// A button - Jump (mapped to Z key)
	keyboard_state[BACKEND_KEYBOARD_Z] = (held & PAD_BUTTON_A) != 0;

	// B button - Shoot (mapped to X key)
	keyboard_state[BACKEND_KEYBOARD_X] = (held & PAD_BUTTON_B) != 0;

	// X button - Inventory (mapped to Q key)
	keyboard_state[BACKEND_KEYBOARD_Q] = (held & PAD_BUTTON_X) != 0;

	// Y button - Map (mapped to W key)
	keyboard_state[BACKEND_KEYBOARD_W] = (held & PAD_BUTTON_Y) != 0;

	// L trigger - Weapon switch left (mapped to A key)
	keyboard_state[BACKEND_KEYBOARD_A] = (held & PAD_TRIGGER_L) != 0;

	// R trigger - Weapon switch right (mapped to S key)
	keyboard_state[BACKEND_KEYBOARD_S] = (held & PAD_TRIGGER_R) != 0;

	// Start button - Escape/Menu
	keyboard_state[BACKEND_KEYBOARD_ESCAPE] = (held & PAD_BUTTON_START) != 0;

	// Z button - Alternative function (F2 for settings)
	keyboard_state[BACKEND_KEYBOARD_F2] = (held & PAD_TRIGGER_Z) != 0;
}

void Backend_ShowMessageBox(const char *title, const char *message)
{
	// On GameCube, just print to debug console
	Backend_PrintInfo("MessageBox - %s - %s", title, message);
}

ATTRIBUTE_FORMAT_PRINTF(1, 2) void Backend_PrintError(const char *format, ...)
{
	char message_buffer[0x100];

	va_list argument_list;
	va_start(argument_list, format);
	vsnprintf(message_buffer, sizeof(message_buffer), format, argument_list);
	va_end(argument_list);

	// Print to debug console (USB Gecko or similar)
	printf("ERROR: %s\n", message_buffer);
}

ATTRIBUTE_FORMAT_PRINTF(1, 2) void Backend_PrintInfo(const char *format, ...)
{
	char message_buffer[0x100];

	va_list argument_list;
	va_start(argument_list, format);
	vsnprintf(message_buffer, sizeof(message_buffer), format, argument_list);
	va_end(argument_list);

	// Print to debug console
	printf("INFO: %s\n", message_buffer);
}

unsigned long Backend_GetTicks(void)
{
	u64 current_time = gettime();
	u64 elapsed = diff_ticks(start_time, current_time);

	// Convert to milliseconds
	return ticks_to_millisecs(elapsed);
}

void Backend_Delay(unsigned int ticks)
{
	// Convert milliseconds to timebase ticks and sleep
	u64 wait_ticks = millisecs_to_ticks(ticks);
	u64 start = gettime();

	while (diff_ticks(start, gettime()) < wait_ticks)
	{
		// Busy wait - could be improved with LWP sleep
		VIDEO_WaitVSync();
	}
}

