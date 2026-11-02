import gzip
import os

html_path = os.path.join(os.path.dirname(__file__), "main", "web_page.html")
gz_header_path = os.path.join(os.path.dirname(__file__), "main", "web_page_gz.h")

with open(html_path, "rb") as f:
    raw_bytes = f.read()

# Maximum compression level 9
gz_bytes = gzip.compress(raw_bytes, compresslevel=9)

print(f"Original HTML size: {len(raw_bytes)} bytes")
print(f"Gzip compressed size: {len(gz_bytes)} bytes (ratio: {len(gz_bytes)/len(raw_bytes)*100:.1f}%)")

with open(gz_header_path, "w", encoding="utf-8") as f:
    f.write("#pragma once\n\n")
    f.write("#include <stdint.h>\n#include <stddef.h>\n\n")
    f.write("// Pre-compressed Glassmorphic Web UI stored strictly in Flash .rodata (Zero RAM usage)\n")
    f.write("static const uint8_t PAGE_GZ[] = {\n")
    for i, b in enumerate(gz_bytes):
        if i % 16 == 0:
            f.write("  ")
        f.write(f"0x{b:02x}, ")
        if (i + 1) % 16 == 0:
            f.write("\n")
    if len(gz_bytes) % 16 != 0:
        f.write("\n")
    f.write("};\n\n")
    f.write(f"static const size_t PAGE_GZ_LEN = {len(gz_bytes)};\n")

print(f"Generated {gz_header_path} successfully!")

