import struct
d = open(r'C:\itc-work\extracted\default.xex', 'rb').read()
count = struct.unpack('>I', d[0x14:0x18])[0]
print('optional headers:', count)
hdr = {}
for i in range(count):
    key, val = struct.unpack('>II', d[0x18 + i * 8:0x20 + i * 8])
    hdr[key] = val
    print('  key %08X value %08X' % (key, val))
if 0x20104 in hdr:
    off = hdr[0x20104]
    slots, raw_addr, data_size, raw_size = struct.unpack('>IIII', d[off:off + 16])
    print('TLS info: slot_count=%u raw_data_address=%08X data_size=%X raw_data_size=%X' % (slots, raw_addr, data_size, raw_size))
    img = open(r'C:\itc-work\image.bin', 'rb').read()
    tpl = img[raw_addr - 0x82000000: raw_addr - 0x82000000 + max(data_size, raw_size)]
    print('template bytes (first 0x40):', tpl[:0x40].hex(' '))
