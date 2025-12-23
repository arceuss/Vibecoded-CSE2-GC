#!/usr/bin/env python3
"""
Generate C headers for all data files to embed them in the GameCube executable.
This creates:
1. Individual .h files for each data file
2. A master DataFiles.cpp with the file table
3. A DataFiles.h header for accessing embedded files
"""

import os
import sys

def sanitize_name(path):
    """Convert a file path to a valid C identifier."""
    name = path.replace('/', '_').replace('\\', '_').replace('.', '_').replace('-', '_')
    return 'data_' + name

def generate_header(input_path, output_path):
    """Generate a header file containing the file data as a byte array."""
    with open(input_path, 'rb') as f:
        data = f.read()
    
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    
    with open(output_path, 'w') as f:
        # Write comma-separated bytes, 16 per line
        for i, byte in enumerate(data):
            if i > 0:
                f.write(',')
            if i % 16 == 0:
                f.write('\n')
            f.write(f'{byte}')
        f.write('\n')

def collect_files(data_dir):
    """Recursively collect all files in the data directory."""
    files = []
    for root, dirs, filenames in os.walk(data_dir):
        for filename in filenames:
            full_path = os.path.join(root, filename)
            rel_path = os.path.relpath(full_path, data_dir)
            files.append(rel_path)
    return sorted(files)

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <data_dir> <output_dir>")
        sys.exit(1)
    
    data_dir = sys.argv[1]
    output_dir = sys.argv[2]
    
    if not os.path.isdir(data_dir):
        print(f"Error: {data_dir} is not a directory")
        sys.exit(1)
    
    files = collect_files(data_dir)
    print(f"Found {len(files)} files to embed")
    
    # Generate individual headers
    file_entries = []
    for rel_path in files:
        input_path = os.path.join(data_dir, rel_path)
        header_path = os.path.join(output_dir, 'DataFiles', rel_path + '.h')
        var_name = sanitize_name(rel_path)
        
        print(f"Processing: {rel_path}")
        generate_header(input_path, header_path)
        
        file_size = os.path.getsize(input_path)
        # Normalize path separators to forward slashes for lookup
        lookup_path = rel_path.replace('\\', '/')
        file_entries.append((lookup_path, var_name, header_path, file_size))
    
    # Generate DataFiles.cpp
    cpp_path = os.path.join(output_dir, 'DataFiles.cpp')
    os.makedirs(os.path.dirname(cpp_path), exist_ok=True)
    
    with open(cpp_path, 'w') as f:
        f.write('// Auto-generated embedded data files\n')
        f.write('// Do not edit manually!\n\n')
        f.write('#include "DataFiles.h"\n')
        f.write('#include <stddef.h>\n')
        f.write('#include <string.h>\n\n')
        
        # Include all data arrays
        for lookup_path, var_name, header_path, file_size in file_entries:
            f.write(f'static const unsigned char {var_name}[] = {{\n')
            f.write(f'#include "DataFiles/{lookup_path}.h"\n')
            f.write('};\n\n')
        
        # Generate file table
        f.write('static const EmbeddedFile embedded_files[] = {\n')
        for lookup_path, var_name, header_path, file_size in file_entries:
            # Escape backslashes in path
            escaped_path = lookup_path.replace('\\', '/')
            f.write(f'\t{{"{escaped_path}", {var_name}, {file_size}}},\n')
        f.write('\t{NULL, NULL, 0}\n')
        f.write('};\n\n')
        
        # Generate lookup function
        f.write('const EmbeddedFile* FindEmbeddedFile(const char* path) {\n')
        f.write('\t// Skip leading path components to get relative path\n')
        f.write('\tconst char* rel_path = path;\n')
        f.write('\t// Look for /data/ or \\\\data\\\\ in path\n')
        f.write('\tconst char* data_marker = strstr(path, "/data/");\n')
        f.write('\tif (data_marker) rel_path = data_marker + 6;\n')
        f.write('\tdata_marker = strstr(path, "\\\\data\\\\");\n')
        f.write('\tif (data_marker) rel_path = data_marker + 6;\n')
        f.write('\t\n')
        f.write('\tfor (const EmbeddedFile* file = embedded_files; file->path != NULL; ++file) {\n')
        f.write('\t\t// Case-insensitive comparison\n')
        f.write('\t\tconst char* a = rel_path;\n')
        f.write('\t\tconst char* b = file->path;\n')
        f.write('\t\tbool match = true;\n')
        f.write('\t\twhile (*a && *b) {\n')
        f.write('\t\t\tchar ca = *a, cb = *b;\n')
        f.write('\t\t\tif (ca >= \'A\' && ca <= \'Z\') ca += 32;\n')
        f.write('\t\t\tif (cb >= \'A\' && cb <= \'Z\') cb += 32;\n')
        f.write('\t\t\tif (ca == \'\\\\\') ca = \'/\';\n')
        f.write('\t\t\tif (cb == \'\\\\\') cb = \'/\';\n')
        f.write('\t\t\tif (ca != cb) { match = false; break; }\n')
        f.write('\t\t\t++a; ++b;\n')
        f.write('\t\t}\n')
        f.write('\t\tif (match && *a == 0 && *b == 0) return file;\n')
        f.write('\t}\n')
        f.write('\treturn NULL;\n')
        f.write('}\n')
    
    # Generate DataFiles.h
    h_path = os.path.join(output_dir, 'DataFiles.h')
    with open(h_path, 'w') as f:
        f.write('// Auto-generated embedded data files header\n')
        f.write('#ifndef DATAFILES_H\n')
        f.write('#define DATAFILES_H\n\n')
        f.write('#include <stddef.h>\n\n')
        f.write('typedef struct {\n')
        f.write('\tconst char* path;\n')
        f.write('\tconst unsigned char* data;\n')
        f.write('\tsize_t size;\n')
        f.write('} EmbeddedFile;\n\n')
        f.write('const EmbeddedFile* FindEmbeddedFile(const char* path);\n\n')
        f.write('#endif // DATAFILES_H\n')
    
    print(f"\nGenerated:")
    print(f"  - {len(files)} header files in {output_dir}/DataFiles/")
    print(f"  - {cpp_path}")
    print(f"  - {h_path}")
    print(f"\nAdd DataFiles.cpp to your build and modify File.cpp to use embedded data.")

if __name__ == '__main__':
    main()

