// Embedded data files header for GameCube port
// This provides access to game data embedded in the executable

#ifndef DATAFILES_H
#define DATAFILES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const char* path;
	const unsigned char* data;
	size_t size;
} EmbeddedFile;

// Find an embedded file by path (case-insensitive, handles both / and \)
const EmbeddedFile* FindEmbeddedFile(const char* path);

// Check if embedded data system is available
int HasEmbeddedData(void);

#ifdef __cplusplus
}
#endif

#endif // DATAFILES_H

