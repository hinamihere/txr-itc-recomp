p = 'C:/itc-work/runtime/src/xma.cpp'
s = open(p, encoding='utf-8').read()
old = '''            const uint32_t d0 = rd32(p);
            if (d0 & ((1u << 20) | (1u << 21))) wr32(p, d0 & ~((1u << 20) | (1u << 21)));   // no decoder: consume the input silently'''
new = '''            const uint32_t d0 = rd32(p);
            if (d0 & ((1u << 20) | (1u << 21)))
            {
                // no decoder: consume the input silently (the first contexts are logged in full so the decoder can be checked against real parameters)
                static int logged = 0;
                if (logged++ < 12)
                {
                    const Ctx c = LoadCtx(p);
                    Log("[xma] kick ctx %u: in0 valid=%d packets=%u ptr=%08X | in1 valid=%d packets=%u ptr=%08X | cur=%u readOffset=%u bits (packet %u) | out ptr=%08X blocks=%u write=%u read=%u valid=%d | rate=%u stereo=%d loop count=%u start=%u end=%u | d1=%08X d9=%08X",
                        i, c.in0Valid(), c.in0PacketCount(), c.in0Ptr(), c.in1Valid(), c.in1PacketCount(), c.in1Ptr(), c.currentBuffer(), c.readOffsetBits(), c.readOffsetBits() / kBitsPerPacket,
                        c.outPtr(), c.outBlockCount(), c.outWrite(), c.outRead(), c.outValid(), c.sampleRateId(), c.stereo(), c.loopCount(), c.loopStart(), c.loopEnd(), c.d[1], c.d[9]);
                    const uint8_t* pk = GP(PhysToVirtAddr(c.currentBuffer() ? c.in1Ptr() : c.in0Ptr()));
                    Log("[xma]   first packet header bytes: %02X %02X %02X %02X %02X %02X %02X %02X", pk[0], pk[1], pk[2], pk[3], pk[4], pk[5], pk[6], pk[7]);
                }
                wr32(p, d0 & ~((1u << 20) | (1u << 21)));
            }'''
assert s.count(old) == 1
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
