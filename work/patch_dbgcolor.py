p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()

old_start = s.find('        // Debug: ITC_DEBUG_PS_UV=')
assert old_start >= 0
old_end = s.find('    char tag[64];', old_start)
assert old_end > old_start
new_block = '''        // Debug: ITC_DEBUG_PS_UV=<hex prefix of a pixel shader hash> makes that shader output its first interpolators (colour = frac(t0.xy), frac(t1.x)).
        // ITC_DEBUG_PS_COLORS="<hash prefix>=RRGGBB,..." paints the output of the listed pixel shaders in a solid colour (to see which draws cover which pixels).
        static const char* dbg = getenv("ITC_DEBUG_PS_UV");
        static const char* dbgCol = getenv("ITC_DEBUG_PS_COLORS");
        char hs[24]; snprintf(hs, sizeof hs, "%016llx", (unsigned long long)sc.hash);
        const std::string marker = "\\t} while (false);\\n";
        if (dbg && isPixel && strncmp(hs, dbg, strlen(dbg)) == 0)
        {
            const size_t pos = hlsl.find(marker);
            if (pos != std::string::npos) hlsl.insert(pos + marker.size(), "\\toC0 = float4(frac(iT0.xy), frac(iT1.x), 1.0);\\n");
        }
        if (dbgCol && isPixel)
        {
            const char* q = dbgCol;
            while (*q)
            {
                const char* eq = strchr(q, '=');
                if (!eq) break;
                const size_t n = (size_t)(eq - q);
                unsigned rgb = (unsigned)strtoul(eq + 1, nullptr, 16);
                if (n > 0 && strncmp(hs, q, n) == 0)
                {
                    const size_t pos = hlsl.find(marker);
                    if (pos != std::string::npos)
                    {
                        char ins[96]; snprintf(ins, sizeof ins, "\\toC0 = float4(%g, %g, %g, 1.0);\\n", ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
                        hlsl.insert(pos, ins);
                    }
                }
                const char* comma = strchr(eq, ',');
                if (!comma) break;
                q = comma + 1;
            }
        }
    }
'''
s = s[:old_start] + new_block + s[old_end:]
open(p, 'w', encoding='utf-8').write(s)
print('ok')
