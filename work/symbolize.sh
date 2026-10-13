#!/bin/bash
# usage: symbolize.sh run.log  -> replaces "exe+OFF" / "dll+OFF" in thread stack lines with function names
export PATH="/c/mingw64/bin:$PATH"
cd /c/itc-work/rt_build
nm -n -C itc.exe 2>/dev/null | grep -v " U " > exe.sym
nm -n -C itc_host.dll 2>/dev/null | grep -v " U " > dll.sym
python3 - "/c/itc-work/$1" <<'PY'
import sys,re,bisect
def load(p, base):
    a=[];n=[]
    for l in open(p,errors='ignore'):
        f=l.split(None,2)
        if len(f)==3 and f[1] in 'tTwWdDbB':
            try: a.append(int(f[0],16)-base); n.append(f[2].strip())
            except: pass
    z=sorted(zip(a,n)); return [x for x,_ in z],[y for _,y in z]
exe=load('exe.sym',0x140000000); dll=load('dll.sym',0x180000000)
def sym(tab,off):
    i=bisect.bisect_right(tab[0],off)-1
    return tab[1][i] if i>=0 else '?'
for line in open(sys.argv[1].replace('/c/','C:/',1),errors='ignore'):
    if re.match(r'T\d+:',line):
        out=[]
        for m in re.finditer(r'(exe|dll)\+([0-9A-F]+)',line):
            out.append(sym(exe if m.group(1)=='exe' else dll,int(m.group(2),16)).replace('__imp__','').replace('(PPCContext&, unsigned char*)',''))
        pre=re.split(r' (?:exe|dll)\+',line,1)[0].rstrip(':').rstrip(); print(pre+': '+'  <-  '.join(out))
PY
