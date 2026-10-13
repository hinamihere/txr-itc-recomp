"""Generates itc_host.def from imports.txt: the exact set of symbols itc.exe needs from itc_host.dll.
The exe links against an import library built from this file, so rebuilding the DLL never relinks the exe."""
import pathlib
here = pathlib.Path(__file__).parent
names = ['HostRun']
for line in (here.parent / 'imports.txt').read_text().splitlines():
    line = line.strip()
    if line.startswith('__imp__'):
        names.append(f'_Z{len(line)}{line}R10PPCContextPh')
import tomllib
toml = tomllib.loads((here.parent / 'itc.toml').read_text())
seen = set()
for h in toml.get('midasm_hook', []):
    regs = h.get('registers', [])
    key = (h['name'], tuple(regs))
    if key in seen:
        continue
    seen.add(key)
    sig = ''
    for r in regs:
        sig += 'R14PPCXERRegister' if r == 'xer' else 'R13PPCCRRegister' if r.startswith('cr') else 'R11PPCRegister'
    # conditional hooks return bool but the return type is not part of a plain function's mangled name
    names.append(f"_Z{len(h['name'])}{h['name']}{sig or 'v'}")
(here / 'itc_host.def').write_text('LIBRARY itc_host.dll\nEXPORTS\n' + '\n'.join('    ' + n for n in names) + '\n')
print(len(names), 'exports')
