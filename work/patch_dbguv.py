p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()

# repair the broken insertion from the first attempt (a raw tab/newline ended up inside the string literal)
start = s.find('        // Debug: ITC_DEBUG_PS_UV=')
assert start >= 0, 'debug block not found'
end = s.find('    char tag[64];', start)
assert end > start
new_block = '''        // Debug: ITC_DEBUG_PS_UV=<hex prefix of a pixel shader hash> makes that shader output its first interpolators (colour = frac(t0.xy), frac(t1.x)).
        static const char* dbg = getenv("ITC_DEBUG_PS_UV");
        char hs[24]; snprintf(hs, sizeof hs, "%016llx", (unsigned long long)sc.hash);
        if (dbg && isPixel && strncmp(hs, dbg, strlen(dbg)) == 0)
        {
            const std::string marker = "\\t} while (false);\\n";
            const size_t pos = hlsl.find(marker);
            if (pos != std::string::npos) hlsl.insert(pos + marker.size(), "\\toC0 = float4(frac(iT0.xy), frac(iT1.x), 1.0);\\n");
        }
    }
'''
s = s[:start] + new_block + s[end:]
open(p, 'w', encoding='utf-8').write(s)
print('ok')
