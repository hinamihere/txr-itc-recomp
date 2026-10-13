p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# The mip layout of a cached texture is computed once (ComputeMips walks the page map of every level), not at every lookup.
rep('''    uint32_t levels = 1;
    uint8_t stable = 0;   // consecutive hash comparisons that found the data unchanged (saturating)
};
std::unordered_map<uint64_t, CachedTex> g_textures;''', '''    uint32_t levels = 1;
    uint8_t stable = 0;   // consecutive hash comparisons that found the data unchanged (saturating)
    bool mlValid = false;
    MipLayout ml;         // guest mip layout (depends on the fetch constant only; computed once)
};
std::unordered_map<uint64_t, CachedTex> g_textures;''', 'cachedtex ml')
rep('''    const uint64_t key = Hash64(tf.d, sizeof tf.d) ^ 0x1234;
    CachedTex& ct = g_textures[key];
    const MipLayout ml = ComputeMips(tf);''', '''    const uint64_t key = Hash64(tf.d, sizeof tf.d) ^ 0x1234;
    CachedTex& ct = g_textures[key];
    if (!ct.mlValid) { ct.ml = ComputeMips(tf); ct.mlValid = true; }
    const MipLayout& ml = ct.ml;''', 'use cached ml')
open(p, 'w', encoding='utf-8').write(s)
print('patched part 8')
