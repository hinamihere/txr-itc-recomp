import struct, sys
# usage: python xwb_info.py <file.xwb> [extract_index out.bin]  -- parses an Xbox 360 (big-endian) XACT3 wave bank and optionally dumps one entry's raw data
path = sys.argv[1]
d = open(path, 'rb').read()
sig = d[:4]
be = sig == b'DNBW'
assert sig in (b'WBND', b'DNBW'), sig
E = '>' if be else '<'
ver, = struct.unpack(E + 'I', d[4:8])
segs = [struct.unpack(E + 'II', d[8 + 8 * i:16 + 8 * i]) for i in range(5)]   # sig, version, then 5 (offset, length) segments (version 39)
print('version %d, big-endian=%s' % (ver, be))
names = ['BANKDATA', 'ENTRYMETADATA', 'SEEKTABLE', 'ENTRYNAMES', 'ENTRYWAVEDATA']
for n, s in zip(names, segs):
    print('  %-14s offset %8d length %8d' % (n, s[0], s[1]))
bo, bl = segs[0]
flags, count = struct.unpack(E + 'II', d[bo:bo + 8])
bank_name = d[bo + 8:bo + 72].split(b'\0')[0].decode('latin1')
meta_size, name_size, align, compact = struct.unpack(E + 'IIII', d[bo + 72:bo + 88])
print('flags %08X entries %d name %r metaSize %d nameSize %d alignment %d compact %d' % (flags, count, bank_name, meta_size, name_size, align, compact))
mo, ml = segs[1]
entries = []
for i in range(count):
    o = mo + i * meta_size
    fd, fmt, po, pl, ls, lt = struct.unpack(E + 'IIIIII', d[o:o + 24])
    # format bitfield (LSB first): wFormatTag:2 nChannels:3 nSamplesPerSec:18 wBlockAlign:8 wBitsPerSample:1
    tag = fmt & 3; ch = (fmt >> 2) & 7; rate = (fmt >> 5) & 0x3FFFF; blk = (fmt >> 23) & 0xFF; bps = (fmt >> 31) & 1
    entries.append((fd, tag, ch, rate, blk, bps, po, pl, ls, lt))
    if i < 12 or i == count - 1:
        print('  entry %3d: flags/duration %08X tag %d ch %d rate %d blockAlign %d bps %d  play offset %8d len %8d  loop start %d total %d' % (i, fd, tag, ch, rate, blk, bps, po, pl, ls, lt))
wo, wl = segs[4]
if len(sys.argv) >= 4:
    idx = int(sys.argv[2])
    fd, tag, ch, rate, blk, bps, po, pl, ls, lt = entries[idx]
    open(sys.argv[3], 'wb').write(d[wo + po:wo + po + pl])
    print('extracted entry %d: %d bytes (%d packets of 2048) channels %d rate %d' % (idx, pl, pl // 2048, ch, rate))
    print('first bytes', d[wo + po:wo + po + 16].hex())
