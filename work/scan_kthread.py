import glob, re, collections
pcr = collections.Counter()      # offsets read from r13 (PCR)
kth = collections.Counter()      # offsets read from the KTHREAD loaded via r13+256
rx = re.compile(r'ctx\.r(\d+)\.u64 = PPC_LOAD_U(\d+)\(ctx\.r13\.u32 \+ (\d+)\);')
for f in glob.glob(r'C:\itc-work\out\ppc_recomp.*.cpp'):
    lines = open(f, errors='ignore').read().split('\n')
    for i, l in enumerate(lines):
        m = rx.search(l)
        if not m:
            continue
        reg, bits, off = m.group(1), m.group(2), int(m.group(3))
        pcr[(off, bits)] += 1
        if off == 256:
            pat = re.compile(r'PPC_(?:LOAD|STORE)_U(\d+)\(ctx\.r%s\.u32 \+ (-?\d+)' % reg)
            for l2 in lines[i + 1:i + 7]:
                m2 = pat.search(l2)
                if m2:
                    kth[(int(m2.group(2)), m2.group(1))] += 1
print('PCR offsets (offset,bits): count')
for k, v in sorted(pcr.items()):
    print('  %4d (0x%X) u%s: %d' % (k[0], k[0], k[1], v))
print('KTHREAD offsets read right after loading r13+256:')
for k, v in sorted(kth.items()):
    print('  %4d (0x%X) u%s: %d' % (k[0], k[0], k[1], v))
