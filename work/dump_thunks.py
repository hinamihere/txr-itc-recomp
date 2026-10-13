import struct
data = open(r'C:\Users\日南\Documents\txr-itc-recomp\recomp\TXR.base.exe', 'rb').read()
BASE = 0x82000000


def word(a):
    return struct.unpack('>I', data[a - BASE:a - BASE + 4])[0]


def dec(w):
    op = w >> 26
    if op == 31:
        xo = (w >> 1) & 0x3FF
        names = {231: 'stvx', 103: 'lvx', 487: 'stvxl', 359: 'lvxl'}
        if xo in names:
            return '%s v%d,r%d,r%d' % (names[xo], (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31)
        if xo == 444 or xo == 339:
            return 'or/mfspr'
    if op == 4:
        # VMX128 lvx128/stvx128 : xo bits
        xo = w & 0x3D0
        return 'op4 vmx128-ish %08X' % w
    if w == 0x4E800020:
        return 'blr'
    return '%08X' % w


import sys
start, end = int(sys.argv[1], 16), int(sys.argv[2], 16)
step = int(sys.argv[3]) if len(sys.argv) > 3 else 4
for a in range(start, end, step):
    print('%08X: %08X  %s' % (a, word(a), dec(word(a))))
