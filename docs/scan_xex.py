"""Scan a split Xbox 360 LIVE content package for XEX2 magic + UTF-16 filenames."""
import sys, os, struct, re

BASE = r"C:\itc-recomp\tools\Import Tuner Challenge\555307EE\00007000"
HDR = os.path.join(BASE, "D34A635CDC63F95AE273")
DATA_DIR = os.path.join(BASE, "D34A635CDC63F95AE273.data")

def scan_file(path, label, hits, names, needle=b"XEX2"):
    with open(path, "rb") as f:
        carry = b""
        base_off = 0
        while True:
            chunk = f.read(4 << 20)
            if not chunk:
                break
            data = carry + chunk
            start = 0
            while True:
                i = data.find(needle, start)
                if i < 0:
                    break
                hits.add((label, base_off - len(carry) + i))
                start = i + 1
            # UTF-16LE strings containing .xex / default
            try:
                txt = data.decode("utf-16-le", errors="ignore")
                for m in re.finditer(r"[\x20-\x7E]{4,}", txt):
                    s = m.group()
                    if ".xex" in s.lower() or "default" in s.lower():
                        names.add(s)
            except Exception:
                pass
            carry = data[-16:]
            base_off += len(chunk)

hits, names = set(), set()
scan_file(HDR, "header", hits, names)
for n in range(20):
    p = os.path.join(DATA_DIR, f"Data{n:04d}")
    scan_file(p, f"Data{n:04d}", hits, names)

print("XEX2 hits:")
for h in sorted(hits):
    print("  ", h)
print("filename-ish UTF-16 strings:")
for s in sorted(names):
    print("  ", repr(s))
if not hits:
    print("=> no uncompressed XEX2 found; package likely stores files compressed (LZX)")
