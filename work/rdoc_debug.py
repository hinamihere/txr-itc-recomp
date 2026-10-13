# Run inside RenderDoc:   qrenderdoc.exe --python C:\itc-work\rdoc_debug.py
# Environment: RDOC_CAP=<capture.rdc>  RDOC_PIXEL="x,y"  RDOC_PS="0bbc5532,9473127d"  RDOC_OUT=<dir>
# Finds, via pixel history of the largest float colour target, the first draw at that pixel whose pixel shader name starts with one of the given
# hashes and which changed the colour, then runs RenderDoc's shader debugger on it and writes every register change to <out>/trace.txt.
import os, traceback
import renderdoc as rd

cap_path = os.environ.get('RDOC_CAP', '')
out_dir = os.environ.get('RDOC_OUT', 'C:/itc-work/rdoc/dbg')
cands = [tuple(int(v) for v in c.split(',')) for c in os.environ.get('RDOC_PIXEL', '850,480').split(';') if c]
want = [v for v in os.environ.get('RDOC_PS', '0bbc5532,9473127d,d96c4660').split(',') if v]
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, 'trace.txt'), 'w', encoding='utf-8')


def P(*a):
    print(*a, file=log)
    log.flush()


try:
    try:
        rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    except Exception as e:
        P('InitialiseReplay:', e)
    cap = rd.OpenCaptureFile()
    P('OpenFile', cap.OpenFile(cap_path, '', None))
    r = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = r if isinstance(r, tuple) else (None, r)
    resources = {rdesc.resourceId: rdesc.name for rdesc in controller.GetResources()}
    textures = {t.resourceId: t for t in controller.GetTextures()}

    def walk(acts):
        for a in acts:
            yield a
            for c in walk(a.children):
                yield c

    draws = [a for a in walk(controller.GetRootActions()) if (a.flags & rd.ActionFlags.Drawcall)]
    rt_count = {}
    for a in draws:
        rt = a.outputs[0] if len(a.outputs) else None
        if rt:
            rt_count[rt] = rt_count.get(rt, 0) + 1
    best = None
    for rt, n in rt_count.items():
        t = textures.get(rt)
        if t and t.width >= 1024 and 'FLOAT' in t.format.Name().upper() and (best is None or n > rt_count[best]):
            best = rt
    P('target', resources.get(best, best))
    target_eid = None
    px = py = 0
    for (cx, cy) in cands:
        hist = controller.PixelHistory(best, cx, cy, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        P('--- candidate pixel', cx, cy, 'history length', len(hist))
        for m in hist:
            pre = m.preMod.col.floatValue[:4]
            post = m.postMod.col.floatValue[:4]
            controller.SetFrameEvent(m.eventId, False)
            st = controller.GetPipelineState()
            ps = resources.get(st.GetShader(rd.ShaderStage.Pixel), '?')
            changed = any(abs(a - b) > 1e-6 for a, b in zip(pre, post))
            P('hist eid %d ps=%s pre=%s post=%s changed=%s' % (m.eventId, ps, ['%.3f' % v for v in pre], ['%.3f' % v for v in post], changed))
            if target_eid is None and changed and any(ps.startswith('ps_' + w) for w in want):
                target_eid = m.eventId
                px, py = cx, cy
        if target_eid is not None:
            break
    P('debugging event', target_eid)
    if target_eid is not None:
        controller.SetFrameEvent(target_eid, True)
        try:
            st2 = controller.GetPipelineState()
            refl = st2.GetShaderReflection(rd.ShaderStage.Pixel)
            pipe = st2.GetGraphicsPipelineObject()
            targets = controller.GetDisassemblyTargets(True)
            P('disassembly targets', list(targets))
            for tg in targets:
                txt = controller.DisassembleShader(pipe, refl, tg)
                with open(os.path.join(out_dir, 'disasm_%s.txt' % str(tg).replace(' ', '_').replace('/', '_')), 'w', encoding='utf-8') as df:
                    df.write(txt)
        except Exception as e:
            P('disassembly failed', e)
        trace = controller.DebugPixel(px, py, rd.DebugPixelInputs())
        P('trace debugger:', trace.debugger)
        try:
            for i, sf in enumerate(trace.sourceFiles):
                with open(os.path.join(out_dir, 'source%d.hlsl' % i), 'wb') as sfh:
                    sfh.write(sf.contents.encode('utf-8') if isinstance(sf.contents, str) else bytes(sf.contents))
                P('source file', i, sf.filename, len(sf.contents))
        except Exception as e:
            P('no source files:', e)
        if trace.debugger is not None:
            nstep = 0
            while True:
                states = controller.ContinueDebug(trace.debugger)
                if not states:
                    break
                for s in states:
                    nstep += 1
                    for ch in s.changes:
                        a = ch.after
                        try:
                            vals = list(a.value.f32v[:max(1, a.columns)])
                        except Exception:
                            vals = []
                        try:
                            li = trace.instInfo[s.nextInstruction].lineInfo
                            line = li.lineStart if hasattr(li, 'lineStart') else li.lineNumbers[0]
                        except Exception:
                            line = -1
                        P('%d inst=%d L%d %s = %s' % (nstep, s.nextInstruction, line, a.name, ['%.5g' % v for v in vals]))
            P('steps', nstep)
            controller.FreeTrace(trace)
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    P(traceback.format_exc())
finally:
    log.close()
    os._exit(0)
