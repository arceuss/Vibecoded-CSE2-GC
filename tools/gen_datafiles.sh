#!/bin/bash
# Generate DataFiles.cpp from data headers
# Usage: gen_datafiles.sh <data_dir> <src_dir>

DATA_DIR="$1"
SRC_DIR="$2"
OUTPUT="$SRC_DIR/DataFiles.cpp"

if [ -z "$DATA_DIR" ] || [ -z "$SRC_DIR" ]; then
    echo "Usage: $0 <data_dir> <src_dir>"
    exit 1
fi

echo "Generating $OUTPUT..."

cat > "$OUTPUT" << 'HEADER'
// Auto-generated embedded data files - do not edit!
#include "DataFiles.h"
#include <stddef.h>
#include <string.h>

HEADER

# Generate array declarations
find "$DATA_DIR" -type f | sort | while read filepath; do
    relpath="${filepath#$DATA_DIR/}"
    varname="data_$(echo "$relpath" | sed 's/[\/\.\-]/_/g')"
    echo "static const unsigned char ${varname}[] = {" >> "$OUTPUT"
    echo "#include \"DataFiles/${relpath}.h\"" >> "$OUTPUT"
    echo "};" >> "$OUTPUT"
    echo "" >> "$OUTPUT"
done

# Generate file table
echo "static const EmbeddedFile embedded_files[] = {" >> "$OUTPUT"
find "$DATA_DIR" -type f | sort | while read filepath; do
    relpath="${filepath#$DATA_DIR/}"
    varname="data_$(echo "$relpath" | sed 's/[\/\.\-]/_/g')"
    # Get file size (works on both Linux and macOS)
    if stat --version >/dev/null 2>&1; then
        size=$(stat -c%s "$filepath")
    else
        size=$(stat -f%z "$filepath")
    fi
    echo "	{\"$relpath\", $varname, $size}," >> "$OUTPUT"
done
echo "	{NULL, NULL, 0}" >> "$OUTPUT"
echo "};" >> "$OUTPUT"
echo "" >> "$OUTPUT"

# Add lookup functions
cat >> "$OUTPUT" << 'FUNCTIONS'
static int strcasecmp_path(const char* a, const char* b) {
	while (*a && *b) {
		char ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		if (ca == '\\') ca = '/';
		if (cb == '\\') cb = '/';
		if (ca != cb) return ca - cb;
		++a; ++b;
	}
	return *a - *b;
}

const EmbeddedFile* FindEmbeddedFile(const char* path) {
	const char* rel_path = path;
	const char* p;
	p = strstr(path, "/data/");
	if (p) rel_path = p + 6;
	p = strstr(path, "\\data\\");
	if (p) rel_path = p + 6;
	for (const EmbeddedFile* file = embedded_files; file->path != NULL; ++file) {
		if (strcasecmp_path(rel_path, file->path) == 0) {
			return file;
		}
	}
	return NULL;
}

int HasEmbeddedData(void) { return 1; }
FUNCTIONS

COUNT=$(find "$DATA_DIR" -type f | wc -l)
echo "Generated $OUTPUT with $COUNT embedded files"

