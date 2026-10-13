"""Locate __save/rest gprlr/fpr/vmx thunk addresses in the TXR basefile.
Convention (from xextool's IDC): load as raw binary at 0x82000000 => VA = BASE + offset.
"""
import struct, sys

BASE = 0x82000000
IMG = r"C:\itc-recomp\recomp\TXR.base.exe"
ENTRY = 0x82296C80

data = open(IMG, "rb").read()
def va(off): return BASE + off
def u32(off): return struct.unpack_from(">I", data, off)[0]

# --- identity mapping sanity check at entry point ---
eoff = ENTRY - BASE
w = u32(eoff)
print(f"entry @ {ENTRY:X} fileoff 0x{eoff:X} word=0x{w:08X}  "
      f"({'looks like stwu/mflr-ish' if (w >> 26) in (0x25, 0x17, 0x3E, 0x24) else 'SUSPICIOUS - not code-like'})")

def find_sig(sig_bytes, label, start=0):
    off = data.find(sig_bytes, start)
    if off < 0:
        print(f"{label}: NOT FOUND")
        return None
    print(f"{label}: VA=0x{va(off):X} (off 0x{off:X})")
    return off

# --- GPR: exact signatures from xextool IDC ---
save_gpr = find_sig(bytes.fromhex("F9C1FF68F9E1FF70"), "__savegprlr_14")
rest_gpr = find_sig(bytes.fromhex("E9C1FF68E9E1FF70"), "__restgprlr_14")

# validate 18-slot layout (14..31 at +4 each) for save: each slot = std rX
def validate_seq(off, kind, n=18):
    ok = True
    for i in range(n):
        w = u32(off + i*4)
        # std rD, d(r1): op62=111110, RA=r1(1)
        if kind == "save":
            good = (w >> 26) == 0x3E and ((w >> 16) & 0x1F) == 1
        else:  # ld
            good = (w >> 26) == 0x3A and ((w >> 16) & 0x1F) == 1
        if not good:
            print(f"  seq break at slot {14+i}: word=0x{w:08X}")
            ok = False
            break
    print(f"  {kind} sequence 14..31: {'VALID (18 slots)' if ok else 'broken'}")
    return ok

if save_gpr is not None: validate_seq(save_gpr, "save")
if rest_gpr is not None: validate_seq(rest_gpr, "rest")

# --- FPR + VMX: structural scan in the cluster window near the GPR thunks ---
anchor = save_gpr if save_gpr is not None else 0
win_lo, win_hi = max(0, anchor - 0x4000), min(len(data) - 8, anchor + 0x10000)

def scan_words(lo, hi, pred):
    hits = []
    for off in range(lo & ~3, hi, 4):
        w = u32(off)
        if pred(w):
            hits.append(off)
    return hits

# stfd f14,r1 (op54 FRT14 RA1) then stfd f15,r1 => savefpr_14
def is_stfd(w, frt): return (w >> 26) == 54 and ((w >> 21) & 0x1F) == frt and ((w >> 16) & 0x1F) == 1
def is_lfd(w, frt):  return (w >> 26) == 50 and ((w >> 21) & 0x1F) == frt and ((w >> 16) & 0x1F) == 1

save_fpr = next((o for o in scan_words(win_lo, win_hi, lambda w: is_stfd(w,14)) if is_stfd(u32(o+4),15)), None)
rest_fpr = next((o for o in scan_words(win_lo, win_hi, lambda w: is_lfd(w,14)) if is_lfd(u32(o+4),15)), None)
print(f"__savefpr_14: " + (f"VA=0x{va(save_fpr):X}" if save_fpr else "NOT FOUND"))
print(f"__restfpr_14: " + (f"VA=0x{va(rest_fpr):X}" if rest_fpr else "NOT FOUND"))

# VMX: stvx (op31, XO=231) / stvx128 (XO=759) with VT=14/15 or 64/65 adjacent-ish
def is_vx(w, vt, xo): return (w >> 26) == 31 and ((w >> 1) & 0x3FF) == xo and ((w >> 11) & 0x1F) == ((vt >> 2) & 0x1F) and ((w >> 21) & 0x1F) == (vt & 0x1F)
# VT field split in X-form: bits 6-10 = VT (5 bits) — fix encoding: VT at bits 6..10
def vx(w, vt, xo): return (w >> 26) == 31 and ((w >> 1) & 0x3FF) == xo and ((w >> 6) & 0x1F) == (vt & 0x1F)

for xo, nm in ((231, "stvx/lvx"), (759, "stvx128/lvx128")):
    for a, b, tag in ((14, 15, "14"), (64, 65, "64")):
        hits = [o for o in scan_words(win_lo, win_hi, lambda w, a=a, xo=xo: vx(w, a, xo)) if vx(u32(o+8), b, xo) or vx(u32(o+4), b, xo)]
        if hits:
            print(f"vmx {nm} v{a}→v{b} candidate: VA=0x{va(hits[0]):X}  (xo={xo}, fam {tag})")

print("\nRaw window around savegprlr_14 (first 64 words):")
if save_gpr:
    for i in range(0, 64, 4):
        off = save_gpr + i*4
        ws = " ".join(f"{u32(off+j*4):08X}" for j in range(4))
        print(f"  {va(off):08X}: {ws}")
