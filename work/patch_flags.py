p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)


# a shared accessor for the compile flags
rep('bool CompileHlsl(const std::string& src, const char* target, const char* tag, ID3DBlob** blob)\n{',
    '''// FXC's optimiser miscompiles the big branchy lighting shaders (stale registers leak into the colour -> saturated blue/red surfaces), so shaders are
// compiled without optimisation (the driver optimises the bytecode anyway). Experiments: ITC_SHADER_FLAGS=8192 (IEEE strict), 16384 (level 0), 0 (default FXC).
UINT ShaderCompileFlags()
{
    static const UINT f = getenv("ITC_SHADER_FLAGS") ? (UINT)strtoul(getenv("ITC_SHADER_FLAGS"), nullptr, 0) : (UINT)D3DCOMPILE_SKIP_OPTIMIZATION;
    return f;
}

bool CompileHlsl(const std::string& src, const char* target, const char* tag, ID3DBlob** blob)
{''', 'accessor')

start = s.index('    static const UINT firstFlags = getenv("ITC_SHADER_FLAGS")')
end = s.index('\n', start)
s = s[:start] + '    const UINT firstFlags = ShaderCompileFlags();' + s[end:]
open(p, 'w', encoding='utf-8').write(s)
print('ok')
