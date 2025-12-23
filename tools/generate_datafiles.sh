#!/bin/bash
# Generate DataFiles.cpp from embedded data headers
# Usage: ./generate_datafiles.sh <data_dir> <headers_dir> <output_cpp>

DATA_DIR="$1"
HEADERS_DIR="$2"
OUTPUT_CPP="$3"

if [ -z "$DATA_DIR" ] || [ -z "$HEADERS_DIR" ] || [ -z "$OUTPUT_CPP" ]; then
    echo "Usage: $0 <data_dir> <headers_dir> <output_cpp>"
    exit 1
fi

echo "Generating $OUTPUT_CPP..."

# Start the C++ file
cat > "$OUTPUT_CPP" << 'HEADER'
// Auto-generated embedded data files
// Do not edit manually!

#include "DataFiles.h"
#include <stddef.h>
#include <string.h>

HEADER

# Process all data files
find "$DATA_DIR" -type f | sort | while read -r filepath; do
    relpath="${filepath#$DATA_DIR/}"
    varname="data_$(echo "$relpath" | sed 's/[\/\.\-]/_/g')"
    headerpath="$HEADERS_DIR/${relpath}.h"
    
    if [ -f "$headerpath" ]; then
        echo "static const unsigned char ${varname}[] = {" >> "$OUTPUT_CPP"
        echo "#include \"DataFiles/${relpath}.h\"" >> "$OUTPUT_CPP"
        echo "};" >> "$OUTPUT_CPP"
        echo "" >> "$OUTPUT_CPP"
    fi
done

# Generate file table
echo "static const EmbeddedFile embedded_files[] = {" >> "$OUTPUT_CPP"

find "$DATA_DIR" -type f | sort | while read -r filepath; do
    relpath="${filepath#$DATA_DIR/}"
    varname="data_$(echo "$relpath" | sed 's/[\/\.\-]/_/g')"
    headerpath="$HEADERS_DIR/${relpath}.h"
    filesize=$(stat -c%s "$filepath" 2>/dev/null || stat -f%z "$filepath" 2>/dev/null)
    
    if [ -f "$headerpath" ]; then
        # Use forward slashes in path
        lookuppath=$(echo "$relpath" | sed 's/\\/\//g')
        echo "	{\"${lookuppath}\", ${varname}, ${filesize}}," >> "$OUTPUT_CPP"
    fi
done

echo "	{NULL, NULL, 0}" >> "$OUTPUT_CPP"
echo "};" >> "$OUTPUT_CPP"
echo "" >> "$OUTPUT_CPP"

# Add lookup function
cat >> "$OUTPUT_CPP" << 'FOOTER'

static int strcasecmp_path(const char* a, const char* b) {
	while (*a && *b) {
		char ca = *a, cb = *b;
		// Convert to lowercase
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		// Normalize path separators
		if (ca == '\\') ca = '/';
		if (cb == '\\') cb = '/';
		if (ca != cb) return ca - cb;
		++a; ++b;
	}
	return *a - *b;
}

const EmbeddedFile* FindEmbeddedFile(const char* path) {
	// Skip to relative path after "data/"
	const char* rel_path = path;
	const char* p;
	
	// Look for /data/ or \data\ in path
	p = strstr(path, "/data/");
	if (p) rel_path = p + 6;
	p = strstr(path, "\\data\\");
	if (p) rel_path = p + 6;
	p = strstr(path, "/data\\");
	if (p) rel_path = p + 6;
	p = strstr(path, "\\data/");
	if (p) rel_path = p + 6;
	
	// Search file table
	for (const EmbeddedFile* file = embedded_files; file->path != NULL; ++file) {
		if (strcasecmp_path(rel_path, file->path) == 0) {
			return file;
		}
	}
	return NULL;
}

int HasEmbeddedData(void) {
	return 1;
}
FOOTER

echo "Generated $OUTPUT_CPP"

