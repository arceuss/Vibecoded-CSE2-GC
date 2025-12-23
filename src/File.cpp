// Released under the MIT licence.
// See LICENCE.txt for details.

#include "File.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GAMECUBE
#include "DataFiles.h"
#define FILE_LOG(fmt, ...) printf("[File] " fmt "\n", ##__VA_ARGS__)
#else
#define FILE_LOG(fmt, ...) ((void)0)
#endif

unsigned char* LoadFileToMemory(const char *file_path, size_t *file_size)
{
	unsigned char *buffer = NULL;

	FILE_LOG("LoadFileToMemory: %s", file_path);

#ifdef GAMECUBE
	// Try embedded data first on GameCube
	const EmbeddedFile* embedded = FindEmbeddedFile(file_path);
	if (embedded != NULL)
	{
		FILE_LOG("  -> Found embedded: %zu bytes", embedded->size);
		buffer = (unsigned char*)malloc(embedded->size);
		if (buffer != NULL)
		{
			memcpy(buffer, embedded->data, embedded->size);
			*file_size = embedded->size;
			return buffer;
		}
		FILE_LOG("  -> ERROR: malloc failed!");
	}
	else
	{
		FILE_LOG("  -> Not in embedded data, trying filesystem...");
	}
#endif

	FILE *file = fopen(file_path, "rb");

	if (file != NULL)
	{
		FILE_LOG("  -> Opened from filesystem");
		if (!fseek(file, 0, SEEK_END))
		{
			const long _file_size = ftell(file);

			if (_file_size >= 0)
			{
				rewind(file);
				buffer = (unsigned char*)malloc(_file_size);

				if (buffer != NULL)
				{
					if (fread(buffer, _file_size, 1, file) == 1)
					{
						fclose(file);
						*file_size = (size_t)_file_size;
						FILE_LOG("  -> Loaded %ld bytes from file", _file_size);
						return buffer;
					}

					free(buffer);
				}
			}
		}

		fclose(file);
	}

	FILE_LOG("  -> FAILED to load!");
	return NULL;
}

unsigned short File_ReadBE16(FILE *stream)
{
	unsigned char bytes[2];

	fread(bytes, 2, 1, stream);

	return (bytes[0] << 8) | bytes[1];
}

unsigned long File_ReadBE32(FILE *stream)
{
	unsigned char bytes[4];

	fread(bytes, 4, 1, stream);

	return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}

unsigned short File_ReadLE16(FILE *stream)
{
	unsigned char bytes[2];

	fread(bytes, 2, 1, stream);

	return (bytes[1] << 8) | bytes[0];
}

unsigned long File_ReadLE32(FILE *stream)
{
	unsigned char bytes[4];

	fread(bytes, 4, 1, stream);

	return (bytes[3] << 24) | (bytes[2] << 16) | (bytes[1] << 8) | bytes[0];
}

void File_WriteBE16(unsigned short value, FILE *stream)
{
	for (unsigned int i = 2; i-- != 0;)
		fputc(value >> (8 * i), stream);
}

void File_WriteBE32(unsigned long value, FILE *stream)
{
	for (unsigned int i = 4; i-- != 0;)
		fputc(value >> (8 * i), stream);
}

void File_WriteLE16(unsigned short value, FILE *stream)
{
	for (unsigned int i = 0; i < 2; ++i)
		fputc(value >> (8 * i), stream);
}

void File_WriteLE32(unsigned long value, FILE *stream)
{
	for (unsigned int i = 0; i < 4; ++i)
		fputc(value >> (8 * i), stream);
}
