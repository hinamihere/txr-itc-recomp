import struct, sys
d = open(r'C:\itc-work\image.bin', 'rb').read()
B = 0x82000000


def find(words):
    pat = b''.join(struct.pack('>I', w) for w in words)
    out, i = [], 0
    while True:
        i = d.find(pat, i)
        if i < 0:
            return out
        out.append(B + i)
        i += 4


print('savegprlr_14 (std r14,-0x98(r1); std r15,-0x90(r1)):', [hex(a) for a in find([0xF9C1FF68, 0xF9E1FF70])])
print('restgprlr_14 (ld r14..):', [hex(a) for a in find([0xE9C1FF68, 0xE9E1FF70])])
print('savefpr_14 (stfd f14,-0x90(r12)):', [hex(a) for a in find([0xD9CCFF70, 0xD9ECFF78])])
print('restfpr_14 (lfd f14,-0x90(r12)):', [hex(a) for a in find([0xC9CCFF70, 0xC9ECFF78])])
print('savevmx_14 (li r11,-0x120; stvx v14):', [hex(a) for a in find([0x3960FEE0, 0x7DCB61CE])])
print('restvmx_14 (li r11,-0x120; lvx v14):', [hex(a) for a in find([0x3960FEE0, 0x7DCB60CE])])
print('savevmx_64 (li r11,-0x400; stvx128 v64):', [hex(a) for a in find([0x3960FC00, 0x100B61CB])])
print('restvmx_64 (li r11,-0x400; lvx128 v64):', [hex(a) for a in find([0x3960FC00, 0x100B60CB])])
