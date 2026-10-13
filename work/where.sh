#!/bin/bash
# usage: where.sh <module+offset hex>   -> nearest symbol (guest function) containing the host crash address
export PATH="/c/mingw64/bin:$PATH"
cd /c/itc-work/rt_build
T=$(printf '%016x' $((0x140000000 + 0x$1)))
nm -n itc.exe | grep -v " U " | awk -v t=$T '$1 <= t {last=$0} $1 > t {print last; exit}'
