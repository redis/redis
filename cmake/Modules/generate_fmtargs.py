#!/usr/bin/env python3
import subprocess
import sys

if len(sys.argv) < 3:
    print(f"Usage: {sys.argv[0]} <generator_script> <target_header>")
    sys.exit(1)

gen_script = sys.argv[1]
target_file = sys.argv[2]

output = subprocess.check_output([sys.executable, gen_script], text=True)

with open(target_file, "r") as f:
    content = f.read()

mark = "/* Everything below this line"
idx = content.find(mark)
base = content[:idx] if idx != -1 else content

with open(target_file, "w") as f:
    f.write(base + output)
