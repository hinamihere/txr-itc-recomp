#!/bin/bash
# Fast incremental build of the host DLL (+ exe only if generated code changed). Usage: ./build.sh
export PATH="/c/mingw64/bin:$PATH"
cd /c/itc-work && python3 runtime/gen_stubs.py >/dev/null && cmake --build rt_build -j 8 2>&1 | grep -E "error|FAILED|warning: unused" -A3 | head -40
