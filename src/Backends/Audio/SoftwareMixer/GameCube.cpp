// Released under the MIT licence.
// See LICENCE.txt for details.

// GameCube Audio Software Mixer Backend for CSE2
// Uses AUDIO DMA like the Wii port for reliability

#include "Backend.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <gccore.h>
#include <ogc/audio.h>
#include <ogc/cache.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>

#define SAMPLE_RATE 48000
#define SND_BUFFER_SIZE 4096  // Buffer size in bytes (stereo 16-bit samples)

static void (*parent_callback)(long *stream, size_t frames_total);

static mutex_t sound_mutex;
static mutex_t organya_mutex;

// Double-buffered audio output
static u8 audio_buffer[2][SND_BUFFER_SIZE] ATTRIBUTE_ALIGN(32);
static volatile int audio_index = 0;

// Mix buffer for the parent callback (long samples, stereo)
static long *mix_buffer = NULL;
static size_t mix_buffer_samples;

// DMA callback - called when audio hardware needs more data
static void AudioDMACallback(void)
{
	// Calculate number of samples (buffer is stereo 16-bit)
	size_t num_samples = SND_BUFFER_SIZE / 4;  // 4 bytes per stereo sample
	
	// Clear mix buffer
	memset(mix_buffer, 0, num_samples * sizeof(long) * 2);
	
	// Call parent mixer to fill the buffer
	if (parent_callback != NULL)
	{
		parent_callback(mix_buffer, num_samples);
	}
	
	// Convert from long to s16 with clipping
	s16 *out_buffer = (s16*)audio_buffer[audio_index];
	
	for (size_t i = 0; i < num_samples * 2; i++)
	{
		long sample = mix_buffer[i];
		if (sample > 32767)
			sample = 32767;
		else if (sample < -32768)
			sample = -32768;
		out_buffer[i] = (s16)sample;
	}
	
	// Flush cache for DMA
	DCStoreRange(audio_buffer[audio_index], SND_BUFFER_SIZE);
	
	// Start DMA with this buffer
	AUDIO_InitDMA((u32)audio_buffer[audio_index], SND_BUFFER_SIZE);
	
	// Swap buffers for next time
	audio_index ^= 1;
}

unsigned long SoftwareMixerBackend_Init(void (*callback)(long *stream, size_t frames_total))
{
	printf("[Audio] SoftwareMixerBackend_Init starting...\n");
	
	parent_callback = callback;
	
	// Initialize audio hardware
	printf("[Audio] Calling AUDIO_Init...\n");
	AUDIO_Init(NULL);
	AUDIO_StopDMA();
	AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
	printf("[Audio] AUDIO_Init done, sample rate: 48kHz\n");
	
	// Initialize mutexes
	LWP_MutexInit(&sound_mutex, false);
	LWP_MutexInit(&organya_mutex, false);
	printf("[Audio] Mutexes initialized\n");
	
	// Calculate mix buffer size
	mix_buffer_samples = SND_BUFFER_SIZE / 4;  // stereo 16-bit = 4 bytes per sample
	
	// Allocate mix buffer
	mix_buffer = (long*)malloc(mix_buffer_samples * sizeof(long) * 2);
	if (mix_buffer == NULL)
	{
		printf("[Audio] ERROR: Failed to allocate mix buffer\n");
		return 0;
	}
	printf("[Audio] Mix buffer allocated (%zu samples)\n", mix_buffer_samples);
	
	// Clear audio buffers
	memset(audio_buffer[0], 0, SND_BUFFER_SIZE);
	memset(audio_buffer[1], 0, SND_BUFFER_SIZE);
	DCStoreRange(audio_buffer[0], SND_BUFFER_SIZE);
	DCStoreRange(audio_buffer[1], SND_BUFFER_SIZE);
	
	printf("[Audio] Init complete! Sample rate: %d\n", SAMPLE_RATE);
	return SAMPLE_RATE;
}

void SoftwareMixerBackend_Deinit(void)
{
	// Stop audio
	AUDIO_StopDMA();
	AUDIO_RegisterDMACallback(NULL);
	
	// Free mix buffer
	if (mix_buffer != NULL)
	{
		free(mix_buffer);
		mix_buffer = NULL;
	}
	
	// Destroy mutexes
	LWP_MutexDestroy(sound_mutex);
	LWP_MutexDestroy(organya_mutex);
}

bool SoftwareMixerBackend_Start(void)
{
	printf("[Audio] SoftwareMixerBackend_Start...\n");
	
	if (mix_buffer == NULL)
	{
		printf("[Audio] ERROR: mix_buffer is NULL!\n");
		return false;
	}
	
	// Register DMA callback
	AUDIO_RegisterDMACallback(AudioDMACallback);
	
	// Pre-fill first buffer with silence and start DMA
	memset(audio_buffer[0], 0, SND_BUFFER_SIZE);
	DCStoreRange(audio_buffer[0], SND_BUFFER_SIZE);
	
	AUDIO_InitDMA((u32)audio_buffer[0], SND_BUFFER_SIZE);
	AUDIO_StartDMA();
	
	printf("[Audio] SoftwareMixerBackend_Start complete!\n");
	return true;
}

void SoftwareMixerBackend_LockMixerMutex(void)
{
	LWP_MutexLock(sound_mutex);
}

void SoftwareMixerBackend_UnlockMixerMutex(void)
{
	LWP_MutexUnlock(sound_mutex);
}

void SoftwareMixerBackend_LockOrganyaMutex(void)
{
	LWP_MutexLock(organya_mutex);
}

void SoftwareMixerBackend_UnlockOrganyaMutex(void)
{
	LWP_MutexUnlock(organya_mutex);
}
