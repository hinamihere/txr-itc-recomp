# Run inside RenderDoc:   qrenderdoc.exe --python C:\itc-work\rdoc_vertex.py
# Environment: RDOC_CAP=<capture.rdc>  RDOC_PS="d96c4660,9473127d,0bbc5532"  RDOC_VERTS="0,1,2,10,100"  RDOC_OUT=<dir>
# For the first draw of each listed pixel shader, runs the vertex shader debugger for the given vertex ids and prints the final value of every
# output register (o0 = position, o1.. = TEXCOORD0..) into <out>/vertex.txt.
import os, traceback
import renderdoc as rd

cap_path = os.environ.get('RDOC_CAP', '')
out_dir = os.environ.get('RDOC_OUT', 'C:/itc-work/rdoc/vtx')
want = [v for v in os.environ.get('RDOC_PS', 'd96c4660').split(',') if v]
verts = [int(v) for v in os.environ.get('RDOC_VERTS', '0,1,2').split(',') if v]
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, 'vertex.txt'), 'w', encoding='utf-8')


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

    def walk(acts):
        for a in acts:
            yield a
            for c in walk(a.children):
                yield c

    draws = [a for a in walk(controller.GetRootActions()) if (a.flags & rd.ActionFlags.Drawcall)]
    done = set()
    for a in draws:
        controller.SetFrameEvent(a.eventId, False)
        st = controller.GetPipelineState()
        ps = resources.get(st.GetShader(rd.ShaderStage.Pixel), '?')
        key = None
        for w in want:
            if ps.startswith('ps_' + w):
                key = w
        if key is None or key in done:
            continue
        done.add(key)
        P('=== draw eid %d ps=%s vs=%s numIndices=%d' % (a.eventId, ps, resources.get(st.GetShader(rd.ShaderStage.Vertex), '?'), a.numIndices))
        for vid in verts:
            if vid >= a.numIndices:
                continue
            try:
                trace = controller.DebugVertex(vid, 0, vid, 0)
            except Exception as e:
                P('  DebugVertex failed', e)
                continue
            if trace.debugger is None:
                P('  no debugger for vertex', vid)
                continue
            last = {}
            nstep = 0
            while True:
                states = controller.ContinueDebug(trace.debugger)
                if not states:
                    break
                for s in states:
                    nstep += 1
                    for ch in s.changes:
                        a2 = ch.after
                        nm = a2.name
                        if len(nm) > 1 and nm[0] == 'o' and nm[1].isdigit():
                            try:
                                last[nm] = ['%.5g' % v for v in a2.value.f32v[:max(1, a2.columns)]]
                            except Exception:
                                pass
            P('  vertex %d (%d steps):' % (vid, nstep))
            for nm in sorted(last, key=lambda n: (len(n), n)):
                P('     %s = %s' % (nm, last[nm]))
            controller.FreeTrace(trace)
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    P(traceback.format_exc())
finally:
    log.close()
    os._exit(0)
