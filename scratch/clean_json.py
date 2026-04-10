import json
import os

filepath = "/Users/phanbadu/Documents/PlatformIO/Projects/AgriSense IoT/node-red.json"

with open(filepath, 'r', encoding='utf-8') as f:
    lines = f.readlines()

new_lines = [line for line in lines if not line.strip().startswith("//")]

with open(filepath, 'w', encoding='utf-8') as f:
    f.writelines(new_lines)

print(f"Removed {len(lines) - len(new_lines)} comment lines.")
