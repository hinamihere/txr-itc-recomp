# Run inside RenderDoc:   qrenderdoc.exe --python C:\itc-work\rdoc_pixel.py
# Environment: RDOC_CAP=<capture.rdc>  RDOC_PROBES="x,y;x,y" (pixels of the largest HDR scene target)  RDOC_OUT=<output dir>
# Writes <out>/analysis.txt: the draw summary, shader names per draw, and the pixel history of each probe.
import os, sys, traceback
import renderdoc as rd

cap_path = os.environ.get('RDOC_CAP', '')
out_dir = os.environ.get('RDOC_OUT', 'C:/itc-work/rdoc/out')
probes = [tuple(int(v) for v in p.split(',')) for p in os.environ.get('RDOC_PROBES', '160,480').split(';') if p]
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, 'analysis.txt'), 'w', encoding='utf-8')


def P(*a):
    print(*a, file=log)
    log.flush()


def ok(res):
    try:
        return res.OK()
    except Exception:
        try:
            return res == rd.ReplayStatus.Succeeded
        except Exception:
            return True


try:
    try:
        rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    except Exception as e:
        P('InitialiseReplay:', e)
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(cap_path, '', None)
    P('OpenFile', cap_path, res)
    if not ok(res):
        raise RuntimeError('OpenFile failed')
    r = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = r if isinstance(r, tuple) else (None, r)
    P('OpenCapture', status)
    sf = controller.GetStructuredFile()
    resources = {}
    for rdesc in controller.GetResources():
        resources[rdesc.resourceId] = rdesc.name
    textures = {t.resourceId: t for t in controller.GetTextures()}

    def walk(acts):
        for a in acts:
            yield a
            for c in walk(a.children):
                yield c

    roots = controller.GetRootActions()
    allacts = list(walk(roots))
    draws = [a for a in allacts if (a.flags & rd.ActionFlags.Drawcall)]
    P('actions', len(allacts), 'draws', len(draws))

    # per-draw: output target, shaders
    rt_count = {}
    draw_info = []
    for a in draws:
        rt = a.outputs[0] if len(a.outputs) else rd.ResourceId.Null()
        rt_count[rt] = rt_count.get(rt, 0) + 1
    P('render targets used by draws:')
    for rt, n in sorted(rt_count.items(), key=lambda kv: -kv[1]):
        t = textures.get(rt)
        P('  ', resources.get(rt, str(rt)), n, 'draws', ('%dx%d fmt=%s' % (t.width, t.height, t.format.Name())) if t else '')

    # the main scene target = most-drawn target of width >= 1040 with a float format
    best = None
    for rt, n in rt_count.items():
        t = textures.get(rt)
        if t and t.width >= 1024 and 'FLOAT' in t.format.Name().upper() and (best is None or n > rt_count[best]):
            best = rt
    P('main scene target:', best, resources.get(best, ''))

    # full draw list with PS / VS names (needs one replay per draw: only done for draws into the main target)
    eid_name = {}
    shader_of = {}
    for a in draws:
        rt = a.outputs[0] if len(a.outputs) else None
        if rt != best:
            continue
        controller.SetFrameEvent(a.eventId, False)
        st = controller.GetPipelineState()
        try:
            ps = resources.get(st.GetShader(rd.ShaderStage.Pixel), '?')
            vs = resources.get(st.GetShader(rd.ShaderStage.Vertex), '?')
        except Exception as e:
            ps = vs = '?(%s)' % e
        shader_of[a.eventId] = (vs, ps)
        eid_name[a.eventId] = a.GetName(sf)
    P('draws into the main target: %d' % len(shader_of))
    for eid in sorted(shader_of):
        P('  eid %d  %s  vs=%s ps=%s' % (eid, eid_name[eid], shader_of[eid][0], shader_of[eid][1]))

    for (x, y) in probes:
        P('=== pixel history of (%d,%d) on %s ===' % (x, y, resources.get(best, best)))
        try:
            hist = controller.PixelHistory(best, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        except Exception as e:
            P('PixelHistory failed:', e)
            continue
        for m in hist:
            flags = []
            for f in ('sampleMasked', 'backfaceCulled', 'depthClipped', 'depthBoundsFailed', 'viewClipped', 'scissorClipped', 'shaderDiscarded', 'depthTestFailed', 'stencilTestFailed', 'unboundPS'):
                if getattr(m, f, False):
                    flags.append(f)
            pre = m.preMod.col.floatValue[:4]
            post = m.postMod.col.floatValue[:4]
            sh = shader_of.get(m.eventId)
            try:
                dpre, dsh, dpost = m.preMod.depth, m.shaderOut.depth, m.postMod.depth
            except Exception:
                dpre = dsh = dpost = -1.0
            P('  eid %d %s vs=%s ps=%s  pre=(%.3f %.3f %.3f %.3f) post=(%.3f %.3f %.3f %.3f) depth pre=%.9f shaderOut=%.9f post=%.9f %s' % (
                m.eventId, eid_name.get(m.eventId, ''), sh[0] if sh else '?', sh[1] if sh else '?', pre[0], pre[1], pre[2], pre[3], post[0], post[1], post[2], post[3], dpre, dsh, dpost, ' '.join(flags)))
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    P(traceback.format_exc())
finally:
    log.close()
    os._exit(0)
