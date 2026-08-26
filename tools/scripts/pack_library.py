import os
import json

lib_dir = r"D:\esp32-projects\esp_bytebeat\test_apps\console\dollchan_library"
output_js = os.path.join(lib_dir, "library_data.js")

library_data = []

# Add a default silence item
library_data.append({
    "category": "System",
    "name": "Silence",
    "author": "OmniBox",
    "mode": 0,
    "speed": 1.0,
    "code": "0"
})

categories = ["classic", "js-256", "js-1k", "js-big", "floatbeat", "floatbeat-big", "funcbeat", "recent", "all"]

for root, dirs, files in os.walk(lib_dir):
    for file in files:
        if file.endswith(".txt"):
            filepath = os.path.join(root, file)
            category = os.path.basename(root)
            
            with open(filepath, 'r', encoding='utf-8') as f:
                content = f.read()
            
            name = "Unknown"
            author = "Unknown"
            sr = 8000
            mode_str = "bytebeat"
            
            lines = content.split('\n')
            code_lines = []
            in_code = False
            
            for line in lines:
                if line.startswith("// Name:"):
                    name = line.split(":", 1)[1].strip()
                elif line.startswith("// Author:"):
                    author = line.split(":", 1)[1].strip()
                elif line.startswith("// Sample Rate:"):
                    try:
                        sr = int(line.split(":", 1)[1].strip())
                    except:
                        pass
                elif line.startswith("// Category:"):
                    pass
                elif line.startswith("// Mode:"):
                    mode_str = line.split(":", 1)[1].strip().lower()
                else:
                    if line.startswith("//"):
                        # Skip other comments at the top? Wait, some code might have comments!
                        # We just include the full content as the code so the UI can parse the headers!
                        pass
            
            # The Web UI parses the // Mode and // Sample Rate directly from the code!
            # So we just pass the entire file content as `code`!
            
            # Determine mode integer for backward compatibility if needed, but UI reads from comments anyway now!
            mode_int = 0
            if 'signed bytebeat' in mode_str:
                mode_int = 2
            elif 'floatbeat' in mode_str or 'funcbeat' in mode_str:
                mode_int = 1
                
            speed = round(sr / 48000.0, 4)
            
            library_data.append({
                "category": category,
                "name": name,
                "author": author,
                "mode": mode_int,
                "speed": speed,
                "code": content
            })

# Write to JS
with open(output_js, 'w', encoding='utf-8') as f:
    f.write("const LIBRARY = ")
    json.dump(library_data, f, indent=2, ensure_ascii=False)
    f.write(";\n")

print(f"Packed {len(library_data)} songs into {output_js}")
