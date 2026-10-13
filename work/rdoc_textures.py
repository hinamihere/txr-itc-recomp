# Run inside RenderDoc:   qrenderdoc.exe --python C:\itc-work\rdoc_textures.py
# Environment: RDOC_CAP=<capture.rdc>  RDOC_EIDS="32259,32045"  RDOC_OUT=<output dir>
# For each event: lists the bound pixel-stage textures (slot, name, size, format) and saves each as a PNG; also saves the colour output target after the event.
import os, traceback
import renderdoc as rd

cap_path = os.environ.get('RDOC_CAP', '')
out_dir = os.environ.get('RDOC_OUT', 'C:/itc-work/rdoc/tex')
eids = [int(v) for v in os.environ.get('RDOC_EIDS', '').split(',') if v]
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, 'textures.txt'), 'w', encoding='utf-8')


def P(*a):
    print(*a, file=log)
    log.flush()


def save(controller, rid, path):
    ts = rd.TextureSave()
    ts.resourceId = rid
    ts.destType = rd.FileType.PNG
    ts.mip = 0
    ts.slice.sliceIndex = 0
    ts.alpha = rd.AlphaMapping.Discard
    try:
        controller.SaveTexture(ts, path)
        return True
    except Exception as e:
        P('  save failed', path, e)
        return False


try:
    try:
        rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    except Exception as e:
        P('InitialiseReplay:', e)
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(cap_path, '', None)
    P('OpenFile', res)
    r = cap.OpenCapture(rd.ReplayOptions(), None)
    status, controller = r if isinstance(r, tuple) else (None, r)
    resources = {rdesc.resourceId: rdesc.name for rdesc in controller.GetResources()}
    textures = {t.resourceId: t for t in controller.GetTextures()}
    for eid in eids:
        controller.SetFrameEvent(eid, True)
        st = controller.GetPipelineState()
        P('=== event %d  ps=%s vs=%s' % (eid, resources.get(st.GetShader(rd.ShaderStage.Pixel), '?'), resources.get(st.GetShader(rd.ShaderStage.Vertex), '?')))
        try:
            used = st.GetReadOnlyResources(rd.ShaderStage.Pixel)
        except Exception as e:
            P('GetReadOnlyResources failed', e)
            used = []
        for u in used:
            try:
                idx = u.access.index
                rid = u.descriptor.resource
            except Exception:
                try:
                    idx = u.index
                    rid = u.resourceId
                except Exception as e:
                    P('  unknown descriptor layout', e)
                    continue
            t = textures.get(rid)
            if t is None:
                P('  slot %s: %s (not a texture)' % (idx, resources.get(rid, rid)))
                continue
            path = os.path.join(out_dir, 'e%d_slot%s.png' % (eid, idx))
            ok = save(controller, rid, path)
            P('  slot %s: %s %dx%d mips=%d fmt=%s -> %s' % (idx, resources.get(rid, rid), t.width, t.height, t.mips, t.format.Name(), os.path.basename(path) if ok else 'FAILED'))
        # colour target after this event
        try:
            outs = st.GetOutputTargets()
            for i, o in enumerate(outs[:1]):
                rid = o.resource if hasattr(o, 'resource') else o.descriptor.resource
                path = os.path.join(out_dir, 'e%d_rt%d.png' % (eid, i))
                save(controller, rid, path)
                P('  output target %d: %s -> %s' % (i, resources.get(rid, rid), os.path.basename(path)))
        except Exception as e:
            P('  output target failed', e)
    controller.Shutdown()
    cap.Shutdown()
except Exception:
    P(traceback.format_exc())
finally:
    log.close()
    os._exit(0)
