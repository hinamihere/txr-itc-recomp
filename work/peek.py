import struct, sys
d = open(r'C:\Users\日南\Documents\txr-itc-recomp\recomp\TXR.base.exe', 'rb').read()
for a in sys.argv[1:]:
    a = int(a, 16)
    print(hex(a), hex(struct.unpack('>I', d[a - 0x82000000:a - 0x82000000 + 4])[0]),
          hex(struct.unpack('>I', d[a - 0x82000000 + 4:a - 0x82000000 + 8])[0]))
