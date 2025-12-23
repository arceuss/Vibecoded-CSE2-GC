#!/bin/bash
# Embed all data files using bin2h
# Usage: ./embed_data.sh <bin2h_path> <data_dir> <output_dir>

BIN2H="$1"
DATA_DIR="$2"
OUTPUT_DIR="$3"

if [ -z "$BIN2H" ] || [ -z "$DATA_DIR" ] || [ -z "$OUTPUT_DIR" ]; then
    echo "Usage: $0 <bin2h_path> <data_dir> <output_dir>"
    exit 1
fi

# Create output directories
mkdir -p "$OUTPUT_DIR/DataFiles"

# Find all files and convert them
echo "Embedding data files from $DATA_DIR..."

# Create the file list for the C source
FILE_LIST=""
INCLUDES=""
ENTRIES=""

# Process all files
find "$DATA_DIR" -type f | while read -r filepath; do
    # Get relative path
    relpath="${filepath#$DATA_DIR/}"
    
    # Create safe variable name (replace / . - with _)
    varname="data_$(echo "$relpath" | sed 's/[\/\.\-]/_/g')"
    
    # Create output header path
    outpath="$OUTPUT_DIR/DataFiles/${relpath}.h"
    mkdir -p "$(dirname "$outpath")"
    
    # Convert using bin2h
    echo "  $relpath"
    "$BIN2H" "$filepath" "$outpath"
done

echo "Done! Now run generate_filelist.sh to create DataFiles.cpp"

